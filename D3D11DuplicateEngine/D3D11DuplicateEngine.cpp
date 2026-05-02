#include "pch.h"
#include "D3D11DuplicateEngine.h"
#include "D3D11DuplicateThread.h"

#include "../../../Module/D3D11Engine/Core/D3D11RenderEngine.h"
#include "../../../Module/D3D11ImageIO/D3D11ImageIO/D3D11ImageIO.h"
#include "../../../Module/Core/DirectX/DxSafeRelease.h"  // for SafeRelease

using namespace Core::DirectX;

D3D11DuplicateEngine::~D3D11DuplicateEngine()
{
	Shutdown();
}

bool D3D11DuplicateEngine::Initialize(D3D11RenderEngine* D3D11Engine, uint32_t outputIndex)
{
	if (IsInitialized())
	{
		Shutdown();
	}

	if (D3D11Engine)
	{
		m_D3D11Engine = D3D11Engine;
		m_ownsD3D11Engine = false;
	}
	else
	{
		// Rendering Engine
		RenderEngineConfig renderEngineConfig = {};
		renderEngineConfig.initD2D = false;
		renderEngineConfig.initD3D = true;
#if defined(_DEBUG)
		renderEngineConfig.initDebugLayer = true;
#endif
		renderEngineConfig.initFontManager = false;

		m_D3D11Engine = new D3D11RenderEngine();
		if (!m_D3D11Engine)
			return false;
		m_ownsD3D11Engine = true;

		if (!m_D3D11Engine->Initialize(renderEngineConfig))
		{
			Shutdown();
			return false;
		}

		if (!m_D3D11Engine || !m_D3D11Engine->IsDeviceAvailable())
		{
			Shutdown();
			return false;
		}
	}

	m_WICImageIO = new D3D11ImageIO();
	if (!m_WICImageIO)
	{
		Shutdown();
		return false;
	}

	if (FAILED(m_WICImageIO->Initialize()))
	{
		Shutdown();
		return false;
	}

	HRESULT hr = InitializeDuplication(outputIndex);
	if (FAILED(hr))
	{
		Shutdown();
		return false;
	}

	if (m_enableSharedTexture)
	{
		hr = CreateSharedTexture(m_duplDesc.ModeDesc.Width, m_duplDesc.ModeDesc.Height, &m_sharedTexture, &m_sharedHandle);
		if (FAILED(hr))
		{
			Shutdown();
			return false;
		}
	}

	if (!InitializeCaptureFramePool())
	{
		Shutdown();
		return false;
	}

	m_outputIndex = outputIndex;
	m_initialized = true;

	return true;
}

void D3D11DuplicateEngine::Shutdown()
{
	StopThread();

	DestroyCaptureFramePool();

	if (m_WICImageIO)
	{
		delete m_WICImageIO;
		m_WICImageIO = nullptr;
	}

	SafeRelease(m_dxgiOutput);
	SafeRelease(m_deskDupl);
	SafeRelease(m_capturedTexture);
	SafeRelease(m_sharedTexture);

	m_frameAcquired = false;
	m_sharedHandle = nullptr;

	if (m_mouseInfo.shapeBuffer)
	{
		delete[] m_mouseInfo.shapeBuffer;
		m_mouseInfo.shapeBuffer = nullptr;
	}

	if (m_metaDataBuffer)
	{
		delete[] m_metaDataBuffer;
		m_metaDataBuffer = nullptr;
	}
	m_metaDataSize = 0;

	m_mouseInfo.bufferSize = 0;
	m_mouseInfo.visible = false;
	m_mouseInfo.whoUpdatedPositionLast = 0;
	ZeroMemory(&m_mouseInfo.shapeInfo, sizeof(m_mouseInfo.shapeInfo));
	ZeroMemory(&m_mouseInfo.position, sizeof(m_mouseInfo.position));
	ZeroMemory(&m_mouseInfo.lastTimeStamp, sizeof(m_mouseInfo.lastTimeStamp));

	if (m_ownsD3D11Engine && m_D3D11Engine)
	{
		delete m_D3D11Engine;
	}
	m_D3D11Engine = nullptr;
	m_ownsD3D11Engine = false;

	m_initialized = false;
}

void D3D11DuplicateEngine::SetTargetFps(uint64_t fps)
{
	m_captureFPS = fps;
}

uint64_t D3D11DuplicateEngine::GetTargetFps() const
{
	return m_captureFPS;
}

uint32_t D3D11DuplicateEngine::GetOutputCount() const
{
	if (!m_D3D11Engine)
		return 0;

	IDXGIFactory2* factory = m_D3D11Engine->GetDXGIFactory();
	IDXGIAdapter1* adapter = nullptr;
	uint32_t count = 0;

	if (SUCCEEDED(factory->EnumAdapters1(0, &adapter)))
	{
		IDXGIOutput* output = nullptr;
		while (adapter->EnumOutputs(count, &output) != DXGI_ERROR_NOT_FOUND)
		{
			count++;
			SafeRelease(output);
		}
		SafeRelease(adapter);
	}

	return count;
}

uint32_t D3D11DuplicateEngine::GetOutputWidth()
{
	if (!m_D3D11Engine)
		return 0;

	if (!IsInitialized())
		return 0;

	return m_duplDesc.ModeDesc.Width;
}

uint32_t D3D11DuplicateEngine::GetOutputHeight()
{
	if (!m_D3D11Engine)
		return 0;

	if (!IsInitialized())
		return 0;

	return m_duplDesc.ModeDesc.Height;
}

bool D3D11DuplicateEngine::AcquireFrame(UINT timeout_ms, CaptureFrameResult& outResult)
{
	if (!IsInitialized())
		return false;

	outResult.texture = nullptr;
	outResult.sharedHandle = nullptr;
	outResult.frameInfo = {};
	outResult.metaData = nullptr;
	outResult.dirtyCount = 0;
	outResult.moveCount = 0;
	outResult.mouseInfo = m_mouseInfo;

	IDXGIResource* desktopResource = nullptr;
	DXGI_OUTDUPL_FRAME_INFO frameInfo = {};

	// 프레임 획득
	HRESULT hr = m_deskDupl->AcquireNextFrame(timeout_ms, &frameInfo, &desktopResource);
	if (hr == DXGI_ERROR_WAIT_TIMEOUT)
		return true;

	if (FAILED(hr))
	{
		if (hr == DXGI_ERROR_ACCESS_LOST)
		{
			m_initialized = false;
		}
		return false;
	}

	m_frameAcquired = true;

	// 텍스처 변환
	SafeRelease(m_capturedTexture);
	hr = desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&m_capturedTexture));
	SafeRelease(desktopResource);
	if (FAILED(hr) || !m_capturedTexture)
	{
		ReleaseFrame();
		return false;
	}

	// 데이터 업데이트
	if (m_useMouseInfo)
	{
		if (!UpdateMouseInfo(frameInfo))
		{
			ReleaseFrame();
			return false;
		}
	}

	if (m_useMoveDiryInfo)
	{
		if (!UpdateDirtyMoveInfo(frameInfo, outResult))
		{
			ReleaseFrame();
			return false;
		}
	}

	// 로컬 뷰어용 공유 텍스쳐 복사
	if (m_sharedTexture)
	{
		m_D3D11Engine->GetD3DDeviceContext()->CopyResource(m_sharedTexture, m_capturedTexture);
	}

	//if (m_WICImageIO)
	//{
	//	if (FAILED(m_WICImageIO->SaveTextureToFile(
	//		m_D3D11Engine->GetD3DDevice(),
	//		m_D3D11Engine->GetD3DDeviceContext(),
	//		m_capturedTexture,
	//		L"C:\\debug\\desktop.bmp"
	//	)))
	//	{
	//		return false;
	//	}
	//}

	// 결과 저장
	outResult.texture = m_capturedTexture;
	outResult.sharedHandle = m_enableSharedTexture == true ? m_sharedHandle : nullptr;
	outResult.frameInfo = frameInfo;
	if (m_useMouseInfo)
	{
		outResult.mouseInfo = m_mouseInfo;
	}

	return true;
}

void D3D11DuplicateEngine::ReleaseFrame()
{
	if (m_deskDupl && m_frameAcquired)
	{
		HRESULT hr = m_deskDupl->ReleaseFrame();
		m_frameAcquired = false;

		if (FAILED(hr))
			return;
	}

	SafeRelease(m_capturedTexture);
}

HRESULT D3D11DuplicateEngine::InitializeDuplication(uint32_t outputIndex)
{
	HRESULT hr = S_OK;

	// Adapter 가져오기
	IDXGIFactory2* factory = m_D3D11Engine->GetDXGIFactory();
	IDXGIAdapter1* adapter = nullptr;
	if (FAILED(factory->EnumAdapters1(0, &adapter)))
		return E_FAIL;

	// 해당 인덱스 Output(모니터) 찾기
	IDXGIOutput* output = nullptr;
	hr = adapter->EnumOutputs(outputIndex, &output);
	SafeRelease(adapter);
	if (FAILED(hr))
		return hr;

	output->GetDesc(&m_outputDesc);

	// 인터페이스 전환
	hr = output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&m_dxgiOutput));
	SafeRelease(output);
	if (FAILED(hr))
		return hr;

	// Desktop Duplication 초기화
	hr = m_dxgiOutput->DuplicateOutput(m_D3D11Engine->GetD3DDevice(), &m_deskDupl);
	if (FAILED(hr))
	{
		return hr;
	}

	// Output(모니터) 정보 저장
	m_deskDupl->GetDesc(&m_duplDesc);

	return hr;
}

void D3D11DuplicateEngine::SetFrameCaptureCallback(FrameCallback funcCallback, void* userData)
{
	m_frameCallback = funcCallback;
	m_userData = userData;
}

CapturedFrameHandle D3D11DuplicateEngine::GetLatestFrameHandle()
{
	CapturedFrameHandle handle = {};

	if (!IsInitialized())
		return handle;

	for (size_t attempt = 0; attempt < POOL_COUNT; ++attempt)
	{
		const LONG64 latestFrameId = GetLatestFrameID();
		const LONG slotId = GetLatestFrameSlotID();

		if (latestFrameId <= 0 || slotId < 0 || slotId >= static_cast<LONG>(POOL_COUNT))
			return handle;

		CapturedFrameSlot& frameSlot = m_framePool[slotId];
		const LONG status = ::InterlockedCompareExchange(&frameSlot.status, 0, 0);
		const LONG64 slotFrameId = ::InterlockedCompareExchange64(&frameSlot.frameId, 0, 0);

		if (status != FrameStatus::READY || slotFrameId != latestFrameId || !frameSlot.texture)
			continue;

		::InterlockedIncrement(&frameSlot.referenceCount);

		const LONG statusAfter = ::InterlockedCompareExchange(&frameSlot.status, 0, 0);
		const LONG64 slotFrameIdAfter = ::InterlockedCompareExchange64(&frameSlot.frameId, 0, 0);
		const LONG64 latestFrameIdAfter = GetLatestFrameID();
		const LONG latestSlotIdAfter = GetLatestFrameSlotID();

		if (statusAfter == FrameStatus::READY &&
			slotFrameIdAfter == latestFrameId &&
			latestFrameIdAfter == latestFrameId &&
			latestSlotIdAfter == slotId)
		{
			if (!WaitForFrameSlotCopy(frameSlot))
			{
				::InterlockedDecrement(&frameSlot.referenceCount);
				return handle;
			}

			frameSlot.texture->AddRef();
			handle.texture = frameSlot.texture;
			handle.slotId = slotId;
			handle.frameId = static_cast<uint64_t>(latestFrameId);
			return handle;
		}

		::InterlockedDecrement(&frameSlot.referenceCount);
	}

	return handle;
}

void D3D11DuplicateEngine::ReleaseLatestFrameHandle(CapturedFrameHandle& handle)
{
	if (!handle.texture)
		return;

	const LONG slotId = handle.slotId;
	if (slotId < 0 || slotId >= POOL_COUNT)
	{
		handle.texture->Release();
		handle.texture = nullptr;
		handle.slotId = -1;
		handle.frameId = 0ULL;
		__debugbreak();
		return;
	}

	CapturedFrameSlot& frameSlot = m_framePool[slotId];

	handle.texture->Release();

	const LONG referenceCount = ::InterlockedDecrement(&frameSlot.referenceCount);
	if (referenceCount < 0)
	{
		::InterlockedExchange(&frameSlot.referenceCount, 0);
		__debugbreak();
	}

	handle.texture = nullptr;
	handle.slotId = -1;
	handle.frameId = 0ULL;
}

uint64_t D3D11DuplicateEngine::GetDroppedFrameCount()
{
	return static_cast<uint64_t>(::InterlockedCompareExchange64(&m_droppedFrameCount, 0, 0));
}

ID3D11Device1* D3D11DuplicateEngine::GetD3DDevice()
{
	if (!m_D3D11Engine)
	{
		return nullptr;
	}

	return m_D3D11Engine->GetD3DDevice();
}

bool D3D11DuplicateEngine::StartThread()
{
	if (!IsInitialized())
		return false;

	if (m_duplicateThread && m_duplicateThread->IsRunning())
		return false;

	if (!m_duplicateThread)
	{
		m_duplicateThread = new D3D11DuplicateThread(this);
		if (!m_duplicateThread)
			return false;
	}

	return m_duplicateThread->Start();
}

void D3D11DuplicateEngine::StopThread()
{
	if (!m_duplicateThread)
		return;

	m_duplicateThread->Stop();
	delete m_duplicateThread;
	m_duplicateThread = nullptr;
}

void D3D11DuplicateEngine::ProcessCaptureFrame()
{
	CaptureFrameResult captureFrame = {};

	if (!AcquireFrame(500, captureFrame))
	{
		::Sleep(1);
		return;
	}

	if (!captureFrame.texture)
		return;

	CopyCaptureTextureToPool(captureFrame.texture, captureFrame.frameInfo, captureFrame.mouseInfo);

	CaptureCallbackContext* context = static_cast<CaptureCallbackContext*>(m_userData);

	if (m_enableSharedTexture && m_sharedHandle && context && context->sharedData)
	{
		SharedCaptureData* sharedData = context->sharedData;

		::AcquireSRWLockExclusive(&sharedData->lock);
		sharedData->sharedHandle = m_sharedHandle;
		sharedData->newFrame = true;
		::ReleaseSRWLockExclusive(&sharedData->lock);
	}

	if (m_frameCallback && context)
	{
		context->captureFrame = &captureFrame;
		m_frameCallback(m_userData);
	}

	ReleaseFrame();
}

// 로컬 뷰어(ImageViewer.dll)와 Zero-Copy를 위한 텍스처 생성
HRESULT D3D11DuplicateEngine::CreateSharedTexture(UINT width, UINT height, ID3D11Texture2D** texture, HANDLE* sharedHandle)
{
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED; // 핵심: 공유 플래그

	HRESULT hr = m_D3D11Engine->GetD3DDevice()->CreateTexture2D(&desc, nullptr, texture);
	if (FAILED(hr)) return hr;

	IDXGIResource* pDXGIResource = nullptr;
	hr = (*texture)->QueryInterface(__uuidof(IDXGIResource), (void**)&pDXGIResource);
	if (SUCCEEDED(hr))
	{
		hr = pDXGIResource->GetSharedHandle(sharedHandle);
		pDXGIResource->Release();
	}
	return hr;
}

bool D3D11DuplicateEngine::InitializeCaptureFramePool()
{
	::InterlockedExchange64(&m_latestFrameId, 0);
	::InterlockedExchange(&m_latestFrameSlotId, -1);
	::InterlockedExchange64(&m_droppedFrameCount, 0);

	for (size_t i = 0; i < POOL_COUNT; i++)
	{
		CapturedFrameSlot& frameSlot = m_framePool[i];

		::InterlockedExchange64(&frameSlot.frameId, 0);
		frameSlot.frameInfo = {};
		frameSlot.mouseInfo = {};
		::InterlockedExchange(&frameSlot.status, FrameStatus::EMPTY);
		::InterlockedExchange(&frameSlot.referenceCount, 0);
		SafeRelease(frameSlot.texture);
		SafeRelease(frameSlot.copyDoneQuery);

		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = m_duplDesc.ModeDesc.Width;
		desc.Height = m_duplDesc.ModeDesc.Height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.MiscFlags = 0;

		HRESULT hr = m_D3D11Engine->GetD3DDevice()->CreateTexture2D(&desc, nullptr, &frameSlot.texture);
		if (FAILED(hr))
			return false;

		D3D11_QUERY_DESC queryDesc = {};
		queryDesc.Query = D3D11_QUERY_EVENT;
		hr = m_D3D11Engine->GetD3DDevice()->CreateQuery(&queryDesc, &frameSlot.copyDoneQuery);
		if (FAILED(hr))
			return false;
	}

	return true;
}

void D3D11DuplicateEngine::DestroyCaptureFramePool()
{
	::InterlockedExchange64(&m_latestFrameId, 0);
	::InterlockedExchange(&m_latestFrameSlotId, -1);
	::InterlockedExchange64(&m_droppedFrameCount, 0);

	for (size_t i = 0; i < POOL_COUNT; i++)
	{
		CapturedFrameSlot& frameSlot = m_framePool[i];

		::InterlockedExchange(&frameSlot.status, FrameStatus::EMPTY);
		::InterlockedExchange(&frameSlot.referenceCount, 0);
		::InterlockedExchange64(&frameSlot.frameId, 0);
		SafeRelease(frameSlot.texture);
		SafeRelease(frameSlot.copyDoneQuery);
	}
}

void D3D11DuplicateEngine::CopyCaptureTextureToPool(ID3D11Texture2D* capturedTexture, const DXGI_OUTDUPL_FRAME_INFO& frameInfo, const PTR_INFO& mouseInfo)
{
	if (!capturedTexture)
		return;

	const LONG64 nextFrameId = GetLatestFrameID() + 1;
	const LONG latestSlotId = GetLatestFrameSlotID();
	const LONG startSlotId = latestSlotId >= 0 ? ((latestSlotId + 1) & (POOL_COUNT - 1)) : 0;

	for (size_t offset = 0; offset < POOL_COUNT; ++offset)
	{
		const LONG slotId = (startSlotId + static_cast<LONG>(offset)) & (POOL_COUNT - 1);
		CapturedFrameSlot& frameSlot = m_framePool[slotId];

		if (!frameSlot.texture)
			continue;

		const LONG referenceCount = ::InterlockedCompareExchange(&frameSlot.referenceCount, 0, 0);
		if (referenceCount != 0)
			continue;

		const LONG previousStatus = ::InterlockedCompareExchange(&frameSlot.status, 0, 0);
		if (previousStatus == FrameStatus::BUSY)
			continue;

		if (::InterlockedCompareExchange(&frameSlot.status, FrameStatus::BUSY, previousStatus) != previousStatus)
			continue;

		if (::InterlockedCompareExchange(&frameSlot.referenceCount, 0, 0) != 0)
		{
			::InterlockedExchange(&frameSlot.status, previousStatus);
			continue;
		}

		m_D3D11Engine->GetD3DDeviceContext()->CopyResource(frameSlot.texture, capturedTexture);
		if (frameSlot.copyDoneQuery)
		{
			m_D3D11Engine->GetD3DDeviceContext()->End(frameSlot.copyDoneQuery);
		}

		frameSlot.frameInfo = frameInfo;
		frameSlot.mouseInfo = mouseInfo;
		::InterlockedExchange64(&frameSlot.frameId, nextFrameId);
		::InterlockedExchange(&frameSlot.status, FrameStatus::READY);
		::InterlockedExchange(&m_latestFrameSlotId, slotId);
		::InterlockedExchange64(&m_latestFrameId, nextFrameId);
		return;
	}

	::InterlockedIncrement64(&m_droppedFrameCount);
}

LONG64 D3D11DuplicateEngine::GetLatestFrameID()
{
	return ::InterlockedCompareExchange64(&m_latestFrameId, 0, 0);
}

LONG D3D11DuplicateEngine::GetLatestFrameSlotID()
{
	return ::InterlockedCompareExchange(&m_latestFrameSlotId, 0, 0);
}

bool D3D11DuplicateEngine::WaitForFrameSlotCopy(CapturedFrameSlot& frameSlot)
{
	if (!frameSlot.copyDoneQuery)
		return true;

	ID3D11DeviceContext* context = m_D3D11Engine ? m_D3D11Engine->GetD3DDeviceContext() : nullptr;
	if (!context)
		return false;

	for (;;)
	{
		const HRESULT hr = context->GetData(frameSlot.copyDoneQuery, nullptr, 0, 0);
		if (hr == S_OK)
			return true;

		if (hr != S_FALSE)
			return false;

		::Sleep(0);
	}
}

bool D3D11DuplicateEngine::UpdateMouseInfo(DXGI_OUTDUPL_FRAME_INFO& frameInfo)
{
	if (!m_useMouseInfo)
		return true;

	if (frameInfo.LastMouseUpdateTime.QuadPart == 0)
		return true;

	bool UpdatePosition = true;

	// Make sure we don't update pointer position wrongly
	// If pointer is invisible, make sure we did not get an update from another output that the last time that said pointer
	// was visible, if so, don't set it to invisible or update.
	if (!frameInfo.PointerPosition.Visible && (m_mouseInfo.whoUpdatedPositionLast != m_outputIndex))
	{
		UpdatePosition = false;
	}

	// If two outputs both say they have a visible, only update if new update has newer timestamp
	if (frameInfo.PointerPosition.Visible && m_mouseInfo.visible && (m_mouseInfo.whoUpdatedPositionLast != m_outputIndex) && (m_mouseInfo.lastTimeStamp.QuadPart > frameInfo.LastMouseUpdateTime.QuadPart))
	{
		UpdatePosition = false;
	}

	// Update position
	if (UpdatePosition)
	{
		m_mouseInfo.position.x = frameInfo.PointerPosition.Position.x + m_outputDesc.DesktopCoordinates.left;
		m_mouseInfo.position.y = frameInfo.PointerPosition.Position.y + m_outputDesc.DesktopCoordinates.top;
		m_mouseInfo.whoUpdatedPositionLast = m_outputIndex;
		m_mouseInfo.lastTimeStamp = frameInfo.LastMouseUpdateTime;
		m_mouseInfo.visible = frameInfo.PointerPosition.Visible != 0;
	}

	// No new shape
	if (frameInfo.PointerShapeBufferSize == 0)
	{
		return true;
	}

	// Old buffer too small
	if (frameInfo.PointerShapeBufferSize > m_mouseInfo.bufferSize)
	{
		if (m_mouseInfo.shapeBuffer)
		{
			delete[] m_mouseInfo.shapeBuffer;
			m_mouseInfo.shapeBuffer = nullptr;
		}
		m_mouseInfo.shapeBuffer = new BYTE[frameInfo.PointerShapeBufferSize];
		if (!m_mouseInfo.shapeBuffer)
		{
			m_mouseInfo.bufferSize = 0;
			return false;
		}

		// Update buffer size
		m_mouseInfo.bufferSize = frameInfo.PointerShapeBufferSize;
	}

	// Get shape
	UINT BufferSizeRequired;
	HRESULT hr = m_deskDupl->GetFramePointerShape(frameInfo.PointerShapeBufferSize, reinterpret_cast<VOID*>(m_mouseInfo.shapeBuffer), &BufferSizeRequired, &(m_mouseInfo.shapeInfo));
	if (FAILED(hr))
	{
		delete[] m_mouseInfo.shapeBuffer;
		m_mouseInfo.shapeBuffer = nullptr;
		m_mouseInfo.bufferSize = 0;
		return false;
	}

	return true;
}

bool D3D11DuplicateEngine::UpdateDirtyMoveInfo(DXGI_OUTDUPL_FRAME_INFO& frameInfo, CaptureFrameResult& outResult)
{
	if (!m_useMoveDiryInfo)
		return true;

	HRESULT hr = S_OK;

	outResult.metaData = nullptr;
	outResult.dirtyCount = 0;
	outResult.moveCount = 0;

	if (frameInfo.TotalMetadataBufferSize)
	{
		// Old buffer too small
		if (frameInfo.TotalMetadataBufferSize > m_metaDataSize)
		{
			if (m_metaDataBuffer)
			{
				delete[] m_metaDataBuffer;
				m_metaDataBuffer = nullptr;
			}
			m_metaDataBuffer = new BYTE[frameInfo.TotalMetadataBufferSize];
			if (!m_metaDataBuffer)
			{
				m_metaDataSize = 0;
				outResult.moveCount = 0;
				outResult.dirtyCount = 0;
				return false;
			}
			m_metaDataSize = frameInfo.TotalMetadataBufferSize;
		}

		UINT BufSize = frameInfo.TotalMetadataBufferSize;

		// Get move rectangles
		hr = m_deskDupl->GetFrameMoveRects(BufSize, reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(m_metaDataBuffer), &BufSize);
		if (FAILED(hr))
		{
			outResult.moveCount = 0;
			outResult.dirtyCount = 0;
			return false;
		}
		outResult.moveCount = BufSize / sizeof(DXGI_OUTDUPL_MOVE_RECT);

		BYTE* DirtyRects = m_metaDataBuffer + BufSize;
		BufSize = frameInfo.TotalMetadataBufferSize - BufSize;

		// Get dirty rectangles
		hr = m_deskDupl->GetFrameDirtyRects(BufSize, reinterpret_cast<RECT*>(DirtyRects), &BufSize);
		if (FAILED(hr))
		{
			outResult.moveCount = 0;
			outResult.dirtyCount = 0;
			return false;
		}
		outResult.dirtyCount = BufSize / sizeof(RECT);

		outResult.metaData = m_metaDataBuffer;
	}

	return true;
}
