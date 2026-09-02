#include "pch.h"
#include "D3D11DuplicateEngine.h"
#include "D3D11DuplicateThread.h"

#include "../../../Module/D3D11Engine/Core/D3D11RenderEngine.h"
#include "../../../Module/Core/DirectX/DxSafeRelease.h"  // for SafeRelease

using namespace Core::DirectX;

namespace
{
	constexpr UINT64 kCaptureAcquireKey = 0;
	constexpr UINT64 kViewerAcquireKey = 1;

	constexpr UINT kAcquireTimeoutMs = 500;

	// 재연결 백오프. 보안 데스크톱 전환은 보통 1~2 초 안에 끝나므로
	// 짧게 시작해 500ms 까지만 늘린다.
	constexpr uint32_t kReconnectMinDelayMs = 50;
	constexpr uint32_t kReconnectMaxDelayMs = 500;

	// 재연결이 길어질 때 통지 폭주를 막는 간격(시도 횟수 기준).
	constexpr uint32_t kReconnectNotifyInterval = 20;
}

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

		if (m_immediateContextGateSettingExplicit &&
			m_D3D11Engine->IsImmediateContextGateEnabled() != m_immediateContextGateEnabled)
		{
			// An external engine must be configured before it is initialized.
			if (!m_D3D11Engine->SetImmediateContextGateEnabled(m_immediateContextGateEnabled))
			{
				m_D3D11Engine = nullptr;
				return false;
			}
		}
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

		if (!m_D3D11Engine->SetImmediateContextGateEnabled(m_immediateContextGateEnabled))
		{
			Shutdown();
			return false;
		}

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

	// 재연결 경로가 이 값을 쓰므로 duplication 을 열기 전에 확정한다.
	m_outputIndex = outputIndex;

	const HRESULT hr = InitializeDuplication(outputIndex);
	if (FAILED(hr))
	{
		RecordError(hr);
		Shutdown();
		return false;
	}

	if (!CreateFrameResources())
	{
		Shutdown();
		return false;
	}

	m_initialized = true;
	SetCaptureState(CaptureState::Running);

	return true;
}

void D3D11DuplicateEngine::Shutdown()
{
	StopThread();

	// duplication 을 쥔 채로 놓아버리면 표면 소유권이 DWM 에 돌아가지 않는다.
	ReleaseFrame();

	DestroyFrameResources();

	SafeRelease(m_dxgiOutput);
	SafeRelease(m_deskDupl);
	SafeRelease(m_capturedTexture);

	m_frameAcquired = false;

	m_reconnectAttempt = 0;
	m_reconnectDelayMs = kReconnectMinDelayMs;
	m_deviceRemovedNotified = false;
	m_duplDesc = {};
	m_outputDesc = {};

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
	SetCaptureState(CaptureState::Idle);
}

bool D3D11DuplicateEngine::SetCaptureOutputMode(CaptureOutputMode outputMode)
{
	if (IsInitialized())
		return false;

	if (outputMode != CaptureOutputMode::FramePool && outputMode != CaptureOutputMode::SharedTexture)
		return false;

	m_captureOutputMode = outputMode;
	return true;
}

bool D3D11DuplicateEngine::SetWaitForFrameCopyCompletion(bool enabled)
{
	if (IsInitialized())
		return false;

	m_waitForFrameCopyCompletion = enabled;
	return true;
}

bool D3D11DuplicateEngine::SetImmediateContextGateEnabled(bool enabled)
{
	if (IsInitialized())
		return false;

	m_immediateContextGateEnabled = enabled;
	m_immediateContextGateSettingExplicit = true;
	return true;
}

bool D3D11DuplicateEngine::IsImmediateContextGateEnabled() const
{
	return m_D3D11Engine
		? m_D3D11Engine->IsImmediateContextGateEnabled()
		: m_immediateContextGateEnabled;
}

void D3D11DuplicateEngine::SetSkipUnchangedFrames(bool enabled)
{
	::InterlockedExchange(&m_skipUnchangedFrames, enabled ? TRUE : FALSE);
}

bool D3D11DuplicateEngine::IsSkipUnchangedFramesEnabled() const
{
	return ::InterlockedCompareExchange(
		const_cast<volatile LONG*>(&m_skipUnchangedFrames), 0, 0) != FALSE;
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
	{
		::InterlockedIncrement64(&m_timeoutCount);
		return true;
	}

	if (FAILED(hr))
	{
		RecordError(hr);

		// ACCESS_LOST 는 잠금화면, UAC 보안 데스크톱, 해상도 변경, 전체화면 전환,
		// TDR, RDP 연결에서 일상적으로 발생한다. 전부 재연결로 흡수한다.
		// 그 외 실패도 duplication 을 다시 여는 것 말고 할 수 있는 게 없다.
		EnterReconnecting(hr);
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

	// LastPresentTime 이 0 이면 데스크톱 이미지는 직전 프레임과 같다.
	// 마우스만 움직인 경우가 대부분이라 복사할 이유가 없다.
	outResult.desktopUpdated = (frameInfo.LastPresentTime.QuadPart != 0);

	const bool skipThisFrame = !outResult.desktopUpdated && IsSkipUnchangedFramesEnabled();
	if (skipThisFrame)
	{
		::InterlockedIncrement64(&m_skippedFrameCount);

		// 텍스처는 돌려준다. 호출자가 발행 여부를 desktopUpdated 로 판단한다.
		outResult.texture = m_capturedTexture;
		outResult.frameInfo = frameInfo;
		if (m_useMouseInfo)
			outResult.mouseInfo = m_mouseInfo;

		return true;
	}

	bool sharedFrameReady = false;

	// 공유 텍스처는 캡처 장치와 뷰어 장치가 번갈아 소유한다. 뷰어가 아직
	// 이전 프레임을 읽고 있다면 캡처 스레드를 막지 않고 이번 프레임을 버린다.
	if (m_captureOutputMode == CaptureOutputMode::SharedTexture)
	{
		if (!m_sharedTexture || !m_sharedKeyedMutex)
		{
			ReleaseFrame();
			return false;
		}

		const HRESULT acquireHr = m_sharedKeyedMutex->AcquireSync(kCaptureAcquireKey, 0);
		if (acquireHr == WAIT_TIMEOUT)
		{
			::InterlockedIncrement64(&m_droppedFrameCount);
		}
		else if (acquireHr == WAIT_ABANDONED || FAILED(acquireHr))
		{
			ReleaseFrame();
			return false;
		}
		else
		{
			ID3D11DeviceContext* deviceContext = m_D3D11Engine->GetD3DDeviceContext();
			if (!deviceContext)
			{
				m_sharedKeyedMutex->ReleaseSync(kCaptureAcquireKey);
				ReleaseFrame();
				return false;
			}

			{
				D3D11ImmediateContextGuard contextGuard(m_D3D11Engine->GetImmediateContextGate());
				deviceContext->CopyResource(m_sharedTexture, m_capturedTexture);
				deviceContext->Flush();
			}

			const HRESULT releaseHr = m_sharedKeyedMutex->ReleaseSync(kViewerAcquireKey);
			if (FAILED(releaseHr))
			{
				ReleaseFrame();
				return false;
			}

			sharedFrameReady = true;
		}
	}

	// 결과 저장
	outResult.texture = m_capturedTexture;
	outResult.sharedHandle = sharedFrameReady ? m_sharedHandle : nullptr;
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
		// ReleaseFrame transitions ownership of the duplicated desktop resource.
		// Serialize that transition with immediate-context work for the previous
		// frame that may still be running on the encoder thread.
		D3D11ImmediateContextGuard contextGuard(
			m_D3D11Engine ? m_D3D11Engine->GetImmediateContextGate() : nullptr);
		HRESULT hr = m_deskDupl->ReleaseFrame();
		m_frameAcquired = false;

		if (FAILED(hr))
			return;
	}

	SafeRelease(m_capturedTexture);
}

// 재연결 경로에서 반복 호출된다. 실패하면 반드시 자기가 잡은 것을 전부
// 놓고 나가야 다음 시도가 깨끗한 상태에서 시작한다.
HRESULT D3D11DuplicateEngine::InitializeDuplication(uint32_t outputIndex)
{
	if (!m_D3D11Engine)
		return E_FAIL;

	// Adapter 가져오기
	IDXGIFactory2* factory = m_D3D11Engine->GetDXGIFactory();
	if (!factory)
		return E_FAIL;

	IDXGIAdapter1* adapter = nullptr;
	if (FAILED(factory->EnumAdapters1(0, &adapter)) || !adapter)
		return E_FAIL;

	// 해당 인덱스 Output(모니터) 찾기
	IDXGIOutput* output = nullptr;
	HRESULT hr = adapter->EnumOutputs(outputIndex, &output);
	SafeRelease(adapter);
	if (FAILED(hr) || !output)
		return FAILED(hr) ? hr : E_FAIL;

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
		SafeRelease(m_dxgiOutput);
		return hr;
	}

	// Output(모니터) 정보 저장
	m_deskDupl->GetDesc(&m_duplDesc);

	return S_OK;
}

void D3D11DuplicateEngine::SetFrameCaptureCallback(FrameCallback funcCallback, void* userData)
{
	// userData 를 먼저 심고 나서 콜백을 공개한다. 순서가 반대면 캡처 스레드가
	// 콜백은 보고 userData 는 못 본 상태로 호출할 수 있다.
	m_userData = userData;
	::MemoryBarrier();
	m_frameCallback = funcCallback;
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

// 잘못된 반납은 디버거를 붙일 수 없는 서버에서도 일어난다. 예전에는 __debugbreak 로
// 프로세스를 세웠지만, 릴리스 빌드에서도 그대로 살아있어 스트리밍이 통째로 죽었다.
// 이제는 슬롯을 영구히 잠그지 않는 선에서 흡수하고 카운터로만 남긴다.
void D3D11DuplicateEngine::ReleaseLatestFrameHandle(CapturedFrameHandle& handle)
{
	ID3D11Texture2D* texture = handle.texture;
	const LONG slotId = handle.slotId;

	handle.texture = nullptr;
	handle.slotId = -1;
	handle.frameId = 0ULL;

	if (!texture)
		return;

	texture->Release();

	if (slotId < 0 || slotId >= static_cast<LONG>(POOL_COUNT))
	{
		// 이 엔진이 준 핸들이 아니거나 slotId 가 훼손됐다. 참조 카운트는
		// 건드리지 않는다 - 어느 슬롯 것인지 알 수 없기 때문이다.
		::InterlockedIncrement64(&m_invalidReleaseCount);
		return;
	}

	CapturedFrameSlot& frameSlot = m_framePool[slotId];

	if (::InterlockedDecrement(&frameSlot.referenceCount) < 0)
	{
		// 이중 반납. 0 으로 되돌리지 않으면 이 슬롯을 다시는 쓸 수 없다.
		::InterlockedExchange(&frameSlot.referenceCount, 0);
		::InterlockedIncrement64(&m_invalidReleaseCount);
	}
}

void D3D11DuplicateEngine::SetCaptureEventCallback(CaptureEventCallback funcCallback, void* userData)
{
	m_eventUserData = userData;
	::MemoryBarrier();
	m_eventCallback = funcCallback;
}

CaptureState D3D11DuplicateEngine::GetCaptureState() const
{
	return static_cast<CaptureState>(::InterlockedCompareExchange(
		const_cast<volatile LONG*>(&m_captureState), 0, 0));
}

void D3D11DuplicateEngine::SetCaptureState(CaptureState state)
{
	::InterlockedExchange(&m_captureState, static_cast<LONG>(state));
}

void D3D11DuplicateEngine::RecordError(HRESULT hr)
{
	::InterlockedExchange(&m_lastError, static_cast<LONG>(hr));
}

void D3D11DuplicateEngine::NotifyEvent(CaptureEventCode code, HRESULT hr)
{
	// 콜백 포인터는 다른 스레드에서 갈아끼울 수 있다. 한 번만 읽는다.
	const CaptureEventCallback callback = m_eventCallback;
	void* const userData = m_eventUserData;

	if (callback)
	{
		callback(code, hr, userData);
	}
}

CaptureStats D3D11DuplicateEngine::GetStats() const
{
	const auto read64 = [](const volatile LONG64& value) -> uint64_t
	{
		return static_cast<uint64_t>(
			::InterlockedCompareExchange64(const_cast<volatile LONG64*>(&value), 0, 0));
	};

	CaptureStats stats = {};
	stats.capturedFrames = read64(m_capturedFrameCount);
	stats.skippedFrames = read64(m_skippedFrameCount);
	stats.droppedFrames = read64(m_droppedFrameCount);
	stats.timeoutCount = read64(m_timeoutCount);
	stats.accessLostCount = read64(m_accessLostCount);
	stats.reconnectCount = read64(m_reconnectCount);
	stats.deviceRecreateCount = read64(m_deviceRecreateCount);
	stats.invalidReleaseCount = read64(m_invalidReleaseCount);
	stats.lastError = static_cast<HRESULT>(
		::InterlockedCompareExchange(const_cast<volatile LONG*>(&m_lastError), 0, 0));

	return stats;
}

void D3D11DuplicateEngine::DebugSimulateAccessLoss()
{
	// 캡처 스레드가 직접 처리하게 둔다. duplication 객체를 다른 스레드에서
	// 만지면 그 자체가 경쟁 상태가 된다.
	::InterlockedExchange(&m_debugForceAccessLoss, TRUE);
}

void D3D11DuplicateEngine::ResetStats()
{
	::InterlockedExchange64(&m_capturedFrameCount, 0);
	::InterlockedExchange64(&m_skippedFrameCount, 0);
	::InterlockedExchange64(&m_droppedFrameCount, 0);
	::InterlockedExchange64(&m_timeoutCount, 0);
	::InterlockedExchange64(&m_accessLostCount, 0);
	::InterlockedExchange64(&m_reconnectCount, 0);
	::InterlockedExchange64(&m_deviceRecreateCount, 0);
	::InterlockedExchange64(&m_invalidReleaseCount, 0);
	::InterlockedExchange(&m_lastError, S_OK);
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

HANDLE D3D11DuplicateEngine::GetSharedTextureHandle() const
{
	if (!IsInitialized() || m_captureOutputMode != CaptureOutputMode::SharedTexture)
		return nullptr;

	return m_sharedHandle;
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
	switch (GetCaptureState())
	{
	case CaptureState::Running:
		break;

	case CaptureState::Reconnecting:
		RecoverDuplication();
		return;

	case CaptureState::Faulted:
		// 복구 불가. 호출자가 Shutdown/Initialize 로 되살릴 때까지 유휴로 둔다.
		::Sleep(50);
		return;

	default:
		::Sleep(1);
		return;
	}

	if (::InterlockedExchange(&m_debugForceAccessLoss, FALSE) != FALSE)
	{
		EnterReconnecting(DXGI_ERROR_ACCESS_LOST);
		return;
	}

	CaptureFrameResult captureFrame = {};

	if (!AcquireFrame(kAcquireTimeoutMs, captureFrame))
	{
		// 실패 처리(재연결 진입 포함)는 AcquireFrame 안에서 끝났다.
		return;
	}

	if (!captureFrame.texture)
		return;

	// 화면이 그대로면 복사도 발행도 콜백도 하지 않는다.
	// AcquireFrame 이 이미 skippedFrameCount 를 올렸다.
	if (!captureFrame.desktopUpdated && IsSkipUnchangedFramesEnabled())
	{
		ReleaseFrame();
		return;
	}

	if (m_captureOutputMode == CaptureOutputMode::SharedTexture && !captureFrame.sharedHandle)
	{
		ReleaseFrame();
		return;
	}

	if (m_captureOutputMode == CaptureOutputMode::FramePool)
	{
		CopyCaptureTextureToPool(captureFrame.texture, captureFrame.frameInfo, captureFrame.mouseInfo);
	}

	::InterlockedIncrement64(&m_capturedFrameCount);

	// Return the duplicated desktop resource before notifying consumers. The
	// callback may immediately start encoder work on another thread using the
	// same device, which must not overlap the duplication ownership transition.
	ReleaseFrame();

	// 콜백 포인터는 다른 스레드에서 갈아끼울 수 있다. 한 번만 읽는다.
	const FrameCallback frameCallback = m_frameCallback;
	void* const frameUserData = m_userData;
	if (frameCallback)
	{
		frameCallback(frameUserData);
	}
}

// 현재 m_duplDesc 기준으로 출력 리소스를 만든다.
// 재연결 후 해상도가 바뀌었을 때도 같은 경로를 탄다.
bool D3D11DuplicateEngine::CreateFrameResources()
{
	DestroyFrameResources();

	if (!m_D3D11Engine || m_duplDesc.ModeDesc.Width == 0 || m_duplDesc.ModeDesc.Height == 0)
		return false;

	m_frameWidth = m_duplDesc.ModeDesc.Width;
	m_frameHeight = m_duplDesc.ModeDesc.Height;

	if (m_captureOutputMode == CaptureOutputMode::SharedTexture)
	{
		HRESULT hr = CreateSharedTexture(m_frameWidth, m_frameHeight, &m_sharedTexture, &m_sharedHandle);
		if (FAILED(hr))
		{
			RecordError(hr);
			return false;
		}

		hr = m_sharedTexture->QueryInterface(
			__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&m_sharedKeyedMutex));
		if (FAILED(hr) || !m_sharedKeyedMutex)
		{
			RecordError(hr);
			return false;
		}

		return true;
	}

	if (m_captureOutputMode == CaptureOutputMode::FramePool)
		return InitializeCaptureFramePool();

	return false;
}

void D3D11DuplicateEngine::DestroyFrameResources()
{
	DestroyCaptureFramePool();

	SafeRelease(m_sharedKeyedMutex);
	SafeRelease(m_sharedTexture);
	m_sharedHandle = nullptr;

	m_frameWidth = 0;
	m_frameHeight = 0;
}

bool D3D11DuplicateEngine::IsDeviceLost() const
{
	if (!m_D3D11Engine)
		return true;

	ID3D11Device1* device = m_D3D11Engine->GetD3DDevice();
	if (!device)
		return true;

	return FAILED(device->GetDeviceRemovedReason());
}

void D3D11DuplicateEngine::EnterReconnecting(HRESULT hr)
{
	RecordError(hr);

	// duplication 을 쥔 채로 들어오면 재연결 시 놓을 수 없다.
	ReleaseFrame();

	if (GetCaptureState() == CaptureState::Reconnecting)
		return;

	::InterlockedIncrement64(&m_accessLostCount);

	m_reconnectAttempt = 0;
	m_reconnectDelayMs = kReconnectMinDelayMs;

	SetCaptureState(CaptureState::Reconnecting);
	NotifyEvent(CaptureEventCode::AccessLost, hr);
}

void D3D11DuplicateEngine::EnterFaulted(CaptureEventCode code, HRESULT hr)
{
	RecordError(hr);
	ReleaseFrame();

	SetCaptureState(CaptureState::Faulted);
	NotifyEvent(code, hr);
}

void D3D11DuplicateEngine::BackoffReconnectDelay()
{
	::Sleep(m_reconnectDelayMs);

	m_reconnectDelayMs = (m_reconnectDelayMs * 2 < kReconnectMaxDelayMs)
		? m_reconnectDelayMs * 2
		: kReconnectMaxDelayMs;
}

// 디바이스가 사라진 경우. 우리가 만든 엔진일 때만 되살릴 수 있다.
bool D3D11DuplicateEngine::RecreateLostDevice()
{
	HRESULT reason = DXGI_ERROR_DEVICE_REMOVED;
	if (m_D3D11Engine)
	{
		ID3D11Device1* device = m_D3D11Engine->GetD3DDevice();
		if (device)
			reason = device->GetDeviceRemovedReason();
	}

	if (!m_deviceRemovedNotified)
	{
		m_deviceRemovedNotified = true;
		NotifyEvent(CaptureEventCode::DeviceRemoved, reason);
	}

	if (!m_ownsD3D11Engine || !m_D3D11Engine)
	{
		// 외부 엔진의 디바이스는 다른 사용자와 공유된다. 여기서 마음대로
		// 다시 만들면 그쪽 리소스가 전부 깨지므로 호출자에게 넘긴다.
		EnterFaulted(CaptureEventCode::DeviceRemoved, reason);
		return false;
	}

	DestroyFrameResources();
	SafeRelease(m_deskDupl);
	SafeRelease(m_dxgiOutput);

	m_D3D11Engine->DiscardDevice();
	if (!m_D3D11Engine->RecreateDevice())
		return false;

	::InterlockedIncrement64(&m_deviceRecreateCount);
	m_deviceRemovedNotified = false;

	// 이전에 나눠준 텍스처와 공유 핸들은 모두 무효다.
	NotifyEvent(CaptureEventCode::DeviceRecreated, S_OK);
	return true;
}

// 캡처 스레드에서 한 번에 한 번씩만 시도한다. 실패하면 백오프 후 반환해
// 정지 요청에 빠르게 반응할 수 있게 한다.
void D3D11DuplicateEngine::RecoverDuplication()
{
	++m_reconnectAttempt;

	ReleaseFrame();
	SafeRelease(m_deskDupl);
	SafeRelease(m_dxgiOutput);

	if (IsDeviceLost())
	{
		if (!RecreateLostDevice())
		{
			if (GetCaptureState() != CaptureState::Faulted)
				BackoffReconnectDelay();
			return;
		}
	}

	const HRESULT hr = InitializeDuplication(m_outputIndex);
	if (FAILED(hr))
	{
		RecordError(hr);

		// 잠금화면/보안 데스크톱 동안에는 계속 실패한다. 정상이므로 계속 시도하되
		// 통지는 가끔만 해서 폭주를 막는다.
		if ((m_reconnectAttempt % kReconnectNotifyInterval) == 0)
			NotifyEvent(CaptureEventCode::Reconnecting, hr);

		BackoffReconnectDelay();
		return;
	}

	// 잠금화면을 거치는 동안 해상도가 바뀌었을 수 있다.
	if (m_duplDesc.ModeDesc.Width != m_frameWidth ||
		m_duplDesc.ModeDesc.Height != m_frameHeight)
	{
		if (!CreateFrameResources())
		{
			EnterFaulted(CaptureEventCode::Faulted, E_FAIL);
			return;
		}

		NotifyEvent(CaptureEventCode::ModeChanged, S_OK);
	}

	m_reconnectAttempt = 0;
	m_reconnectDelayMs = kReconnectMinDelayMs;
	::InterlockedIncrement64(&m_reconnectCount);

	SetCaptureState(CaptureState::Running);
	NotifyEvent(CaptureEventCode::Reconnected, S_OK);
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
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

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

		if (m_waitForFrameCopyCompletion)
		{
			D3D11_QUERY_DESC queryDesc = {};
			queryDesc.Query = D3D11_QUERY_EVENT;
			hr = m_D3D11Engine->GetD3DDevice()->CreateQuery(&queryDesc, &frameSlot.copyDoneQuery);
			if (FAILED(hr))
				return false;
		}
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

		{
			D3D11ImmediateContextGuard contextGuard(m_D3D11Engine->GetImmediateContextGate());
			ID3D11DeviceContext* context = m_D3D11Engine->GetD3DDeviceContext();
			if (!context)
			{
				::InterlockedExchange(&frameSlot.status, previousStatus);
				return;
			}

			context->CopyResource(frameSlot.texture, capturedTexture);
			if (frameSlot.copyDoneQuery)
			{
				context->End(frameSlot.copyDoneQuery);
			}
			else
			{
				// Without GetData there is no implicit command-buffer submission.
				// Submit the copy before the duplication frame is released while
				// keeping the capture thread non-blocking.
				context->Flush();
			}
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
		HRESULT hr = S_FALSE;
		{
			D3D11ImmediateContextGuard contextGuard(m_D3D11Engine->GetImmediateContextGate());
			hr = context->GetData(frameSlot.copyDoneQuery, nullptr, 0, 0);
		}
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
