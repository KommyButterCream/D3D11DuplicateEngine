#include "pch.h"
#include "D3D11DuplicateEngine.h"
#include "D3D11DuplicateThread.h"

#include "../../../Module/D3D11Engine/Core/D3D11RenderEngine.h"
#include "../../../Module/Core/DirectX/DxSafeRelease.h"  // for SafeRelease

using namespace Core::DirectX;

namespace
{
	constexpr UINT kAcquireTimeout_ms = 500;

	// 재연결 백오프. 보안 데스크톱 전환은 보통 1~2 초 안에 끝나므로
	// 짧게 시작해 500ms 까지만 늘린다.
	constexpr uint32_t kReconnectMinDelay_ms = 50;
	constexpr uint32_t kReconnectMaxDelay_ms = 500;

	// 재연결이 길어질 때 통지 폭주를 막는 간격(시도 횟수 기준).
	constexpr uint32_t kReconnectNotifyInterval = 20;

	// Faulted 상태에서 유휴로 도는 간격.
	constexpr uint32_t kFaultedIdleDelay_ms = 50;
}

// =============================================================================
// 생성 / 소멸
// =============================================================================

D3D11DuplicateEngine::~D3D11DuplicateEngine()
{
	Shutdown();
}

// =============================================================================
// 초기화 전 설정
//
// Initialize 가 만들어 낼 리소스의 모양을 결정하는 설정들.
// 이미 초기화된 뒤에 바꾸면 만들어진 리소스와 어긋나므로 전부 거절한다.
// =============================================================================

bool D3D11DuplicateEngine::SetImmediateContextGateEnabled(bool enabled)
{
	if (IsInitialized())
		return false;

	m_immediateContextGateEnabled = enabled;
	m_immediateContextGateSettingExplicit = true;
	return true;
}

bool D3D11DuplicateEngine::SetWaitForFrameCopyCompletion(bool enabled)
{
	if (IsInitialized())
		return false;

	m_waitForFrameCopyCompletion = enabled;
	return true;
}

// Initialize 전에만 의미가 있다. 이미 초기화된 뒤에 바꾸면 이미 만들어진
// 풀 텍스처와 설정이 어긋나므로 거절한다.
bool D3D11DuplicateEngine::SetFramePoolSharable(bool enabled)
{
	if (m_initialized)
		return false;

	m_framePoolSharable = enabled;
	return true;
}

// =============================================================================
// 초기화 / 종료
//
// Initialize -> InitializeDuplication -> CreateFrameResources 순으로 내려가고,
// Shutdown 이 역순으로 되돌린다.
// =============================================================================

// 어느 단계에서 실패하든 Shutdown 으로 통째로 되돌린다. 반쯤 초기화된 채
// 남겨 두면 다음 Initialize 가 그 위에 또 쌓는다.
bool D3D11DuplicateEngine::Initialize(D3D11RenderEngine* D3D11Engine, uint32_t outputIndex)
{
	if (IsInitialized())
	{
		Shutdown();
	}

	if (D3D11Engine)
	{
		// D3D11Engine 을 외부에서 받아 공유해서 사용하는 경우
		// D3D11DeviceContext 를 공유해서 사용해야하므로
		// D3D11DeviceContext 의 CopyResource 같은 메서드 사용을 위해 Lock 사용.
		m_D3D11Engine = D3D11Engine;
		m_ownsD3D11Engine = false;

		if (m_immediateContextGateSettingExplicit &&
			m_D3D11Engine->IsImmediateContextGateEnabled() != m_immediateContextGateEnabled)
		{
			// 외부 엔진은 초기화되기 전에 설정을 맞춰야 한다.
			if (!m_D3D11Engine->SetImmediateContextGateEnabled(m_immediateContextGateEnabled))
			{
				m_D3D11Engine = nullptr;
				return false;
			}
		}
	}
	else
	{
		// 엔진을 안 받았으면 우리가 만든다. 이 경우에만 디바이스 재생성이 가능하다.
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

// Initialize 중간에 실패한 상태에서도 불리므로, 모든 단계가 "없으면 그냥
// 넘어가는" 형태여야 한다. 스레드를 먼저 세우는 이유는 그 스레드가 아래에서
// 해제할 객체들을 계속 만지고 있기 때문이다.
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
	m_reconnectDelay_ms = kReconnectMinDelay_ms;
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

	::ZeroMemory(&m_mouseInfo.shapeInfo, sizeof(m_mouseInfo.shapeInfo));
	::ZeroMemory(&m_mouseInfo.position, sizeof(m_mouseInfo.position));
	::ZeroMemory(&m_mouseInfo.lastTimeStamp, sizeof(m_mouseInfo.lastTimeStamp));

	if (m_ownsD3D11Engine && m_D3D11Engine)
	{
		delete m_D3D11Engine;
	}
	m_D3D11Engine = nullptr;
	m_ownsD3D11Engine = false;

	m_initialized = false;
	SetCaptureState(CaptureState::Idle);
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

// 현재 m_duplDesc 기준으로 출력 리소스를 만든다.
// 재연결 후 해상도가 바뀌었을 때도 같은 경로를 탄다.
bool D3D11DuplicateEngine::CreateFrameResources()
{
	DestroyFrameResources();

	if (!m_D3D11Engine || m_duplDesc.ModeDesc.Width == 0 || m_duplDesc.ModeDesc.Height == 0)
		return false;

	m_frameWidth = m_duplDesc.ModeDesc.Width;
	m_frameHeight = m_duplDesc.ModeDesc.Height;

	return InitializeCaptureFramePool();
}

void D3D11DuplicateEngine::DestroyFrameResources()
{
	DestroyCaptureFramePool();

	m_frameWidth = 0;
	m_frameHeight = 0;
}

// 슬롯 텍스처를 만든다. 해상도 변경으로 다시 불리는 경우가 있어서 각 슬롯을
// 먼저 비우고 시작한다.
//
// 프레임 ID 와 최신 슬롯도 여기서 0/-1 로 되돌린다. 텍스처가 통째로 바뀌었는데
// 예전 ID 가 남아 있으면 소비자가 낡은 슬롯을 최신으로 착각한다.
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
		SafeRelease(frameSlot.keyedMutex);
		SafeRelease(frameSlot.texture);
		SafeRelease(frameSlot.copyDoneQuery);
		if (frameSlot.sharedHandle)
		{
			::CloseHandle(frameSlot.sharedHandle);
			frameSlot.sharedHandle = nullptr;
		}

		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = m_duplDesc.ModeDesc.Width;
		desc.Height = m_duplDesc.ModeDesc.Height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		
		// 공유 모드면 다른 디바이스가 열 수 있게 만든다.
		// KEYEDMUTEX 를 쓰는 이유는 두 디바이스의 GPU 작업 순서를 드라이버가
		// 아는 객체로 맞추기 위해서다. 앱 레벨 락으로는 그걸 못 한다 —
		// 드라이버가 자기 대기 안에서 그 락을 놓아 줄 수 없기 때문이다.
		// NTHANDLE 로 만든 이유는 해상도 변경 시 풀을 재생성 하는데,
		// 소유권과 CloseHandle 을 명시적으로 드러내기 위해 플래그 사용.
		desc.MiscFlags = m_framePoolSharable
			? (D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE)
			: 0;

		HRESULT hr = m_D3D11Engine->GetD3DDevice()->CreateTexture2D(&desc, nullptr, &frameSlot.texture);
		if (FAILED(hr))
			return false;

		if (m_framePoolSharable)
		{
			// 소비자 디바이스에 건네줄 NT 핸들과, 두 디바이스가 함께 쓸
			// 뮤텍스를 여기서 한 번만 만든다.
			//
			// NT 핸들(IDXGIResource1::CreateSharedHandle)을 쓰는 이유는
			// 수명이 명확하기 때문이다. 예전 방식(GetSharedHandle)의
			// 핸들은 리소스에 묶여 있어 닫을 수도 복제할 수도 없다.
			IDXGIResource1* dxgiResource = nullptr;
			hr = frameSlot.texture->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&dxgiResource));
			if (FAILED(hr) || !dxgiResource)
			{
				RecordError(hr);
				return false;
			}

			hr = dxgiResource->CreateSharedHandle(
				nullptr,
				DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
				nullptr,
				&frameSlot.sharedHandle);
			SafeRelease(dxgiResource);

			if (FAILED(hr) || !frameSlot.sharedHandle)
			{
				RecordError(hr);
				return false;
			}

			hr = frameSlot.texture->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&frameSlot.keyedMutex));
			if (FAILED(hr) || !frameSlot.keyedMutex)
			{
				RecordError(hr);
				return false;
			}
		}

		// 복사 완료 확인용 EVENT 쿼리. 없으면 소비자가 아직 GPU 복사가
		// 끝나지 않은 텍스처를 읽을 수 있다.
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

		SafeRelease(frameSlot.keyedMutex);
		SafeRelease(frameSlot.texture);
		SafeRelease(frameSlot.copyDoneQuery);

		// NT 핸들이라 우리가 닫아야 한다. 소비자가 이미 열어 갔다면
		// 그쪽이 자기 참조를 들고 있으므로 여기서 닫아도 안전하다.
		if (frameSlot.sharedHandle)
		{
			::CloseHandle(frameSlot.sharedHandle);
			frameSlot.sharedHandle = nullptr;
		}
	}
}

// =============================================================================
// 프레임 풀 공유 핸들 (소비자 디바이스 연결)
// =============================================================================

uint32_t D3D11DuplicateEngine::GetFramePoolCount() const
{
	return static_cast<uint32_t>(POOL_COUNT);
}

// 소비자 디바이스가 OpenSharedResource1 로 열 핸들.
//
// 이 핸들의 수명은 엔진이 갖는다. 소비자는 열기만 하고 닫지 않는다 —
// NT 핸들이라 Open 이 자기 참조를 따로 잡으므로 그래도 된다.
// Handle 수명 및 Close 책임은 D3D11DuplicateEngine 이 갖는다.
HANDLE D3D11DuplicateEngine::GetFramePoolSharedHandle(uint32_t slot) const
{
	if (!m_framePoolSharable || slot >= POOL_COUNT)
		return nullptr;

	return m_framePool[slot].sharedHandle;
}

// =============================================================================
// 캡처 스레드 제어
// =============================================================================

// 스레드는 처음 Start 할 때 만든다. Initialize 에서 미리 만들지 않는 이유는
// 소비자가 콜백을 등록하기 전에 프레임이 발행되는 것을 막기 위해서다.
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

// =============================================================================
// 콜백 등록
// =============================================================================

// AcquireFrame 후 캡쳐된 프레임을 풀에 복사한 다음 처리할 외부 함수 등록
void D3D11DuplicateEngine::SetFrameCaptureCallback(FrameCallback funcCallback, void* userData)
{
	m_userData = userData;
	::MemoryBarrier();
	m_frameCallback = funcCallback;
}

// 디바이스 재생성, 풀 재생성, 캡쳐 실패 등 이벤트 발생시 처리할 외부 함수 등록
void D3D11DuplicateEngine::SetCaptureEventCallback(CaptureEventCallback funcCallback, void* userData)
{
	m_eventUserData = userData;
	::MemoryBarrier();
	m_eventCallback = funcCallback;
}

// =============================================================================
// 캡처 루프 (캡처 스레드 전용)
//
// ProcessCaptureFrame 이 한 반복이고 나머지는 그 안에서만 불린다.
// =============================================================================

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
		SleepUnlessStopping(kFaultedIdleDelay_ms);
		return;

	default:
		SleepUnlessStopping(1);
		return;
	}

	if (::InterlockedExchange(&m_debugForceAccessLoss, FALSE) != FALSE)
	{
		EnterReconnecting(DXGI_ERROR_ACCESS_LOST);
		return;
	}

	CaptureFrameResult captureFrame = {};

	if (!AcquireFrame(kAcquireTimeout_ms, captureFrame))
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

	CopyCaptureTextureToPool(captureFrame.texture, captureFrame.frameInfo, captureFrame.mouseInfo);

	// 복사까지 끝난 프레임만 센다. 뒤이은 콜백의 성패는 캡처와 무관하다.
	::InterlockedIncrement64(&m_capturedFrameCount);

	// 소비자에게 알리기 전에 duplication 프레임을 먼저 돌려준다.
	// 콜백이 곧바로 다른 스레드에서 같은 디바이스로 인코딩을 시작할 수 있는데,
	// 그 작업이 duplication 소유권 반환과 겹치면 안 된다.
	ReleaseFrame();

	// 콜백 포인터는 다른 스레드에서 갈아끼울 수 있다. 한 번만 읽는다.
	const FrameCallback frameCallback = m_frameCallback;
	void* const frameUserData = m_userData;
	if (frameCallback)
	{
		frameCallback(frameUserData);
	}
}

// duplication 에서 한 프레임을 받아 outResult 에 채운다.
//
// 반환값은 "캡처 루프를 계속 돌려도 되는가" 다. true 여도 프레임이 없을 수
// 있다(타임아웃). 실패했을 때의 뒷정리와 재연결 진입은 전부 이 안에서 끝낸다.
bool D3D11DuplicateEngine::AcquireFrame(UINT timeout_ms, CaptureFrameResult& outResult)
{
	if (!IsInitialized())
		return false;

	outResult.texture = nullptr;
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
		// 화면이 전혀 바뀌지 않은 것이다. 오류가 아니므로 루프는 계속 돈다.
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

	// 결과 저장
	outResult.texture = m_capturedTexture;
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
		// ReleaseFrame 은 데스크톱 표면의 소유권을 되돌리는 일이다.
		// 인코더 스레드가 아직 이전 프레임으로 immediate context 작업을
		// 돌리고 있을 수 있으므로 그 작업과 순서를 맞춘다.
		D3D11ImmediateContextGuard contextGuard(
			m_D3D11Engine ? m_D3D11Engine->GetImmediateContextGate() : nullptr);
		HRESULT hr = m_deskDupl->ReleaseFrame();
		m_frameAcquired = false;

		if (FAILED(hr))
			return;
	}

	SafeRelease(m_capturedTexture);
}

// duplication 은 커서의 변경분만 준다. 그래서 위치/모양을 m_mouseInfo 에
// 누적해 두고, 매 프레임 그 누적본을 소비자에게 넘긴다.
//
// 아래 위치 갱신 조건이 복잡한 이유는 멀티 모니터 때문이다. 커서는 한
// 화면에만 있는데 모든 출력이 각자 커서 정보를 보고하므로, 다른 모니터가
// 보고한 낡은 상태로 덮어쓰지 않도록 걸러야 한다.
bool D3D11DuplicateEngine::UpdateMouseInfo(DXGI_OUTDUPL_FRAME_INFO& frameInfo)
{
	if (!m_useMouseInfo)
		return true;

	if (frameInfo.LastMouseUpdateTime.QuadPart == 0)
		return true;

	bool UpdatePosition = true;

	// 커서가 우리 화면에 없다는 보고인데 마지막으로 위치를 갱신한 것이
	// 다른 모니터라면, 그쪽이 들고 있는 "보이는 커서" 를 우리가 지우는 셈이다.
	if (!frameInfo.PointerPosition.Visible && (m_mouseInfo.whoUpdatedPositionLast != m_outputIndex))
	{
		UpdatePosition = false;
	}

	// 두 모니터가 모두 "여기 커서가 보인다" 고 하면 더 최근 보고만 반영한다.
	if (frameInfo.PointerPosition.Visible && m_mouseInfo.visible && (m_mouseInfo.whoUpdatedPositionLast != m_outputIndex) && (m_mouseInfo.lastTimeStamp.QuadPart > frameInfo.LastMouseUpdateTime.QuadPart))
	{
		UpdatePosition = false;
	}

	// 위치 갱신
	if (UpdatePosition)
	{
		m_mouseInfo.position.x = frameInfo.PointerPosition.Position.x + m_outputDesc.DesktopCoordinates.left;
		m_mouseInfo.position.y = frameInfo.PointerPosition.Position.y + m_outputDesc.DesktopCoordinates.top;
		m_mouseInfo.whoUpdatedPositionLast = m_outputIndex;
		m_mouseInfo.lastTimeStamp = frameInfo.LastMouseUpdateTime;
		m_mouseInfo.visible = frameInfo.PointerPosition.Visible != 0;
	}

	// 모양이 바뀌지 않았다
	if (frameInfo.PointerShapeBufferSize == 0)
	{
		return true;
	}

	// 버퍼가 모자라면 키운다
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

		// 늘어난 용량 기록
		m_mouseInfo.bufferSize = frameInfo.PointerShapeBufferSize;
	}

	// 커서 비트맵 받기
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

// 이번 프레임에서 바뀐 영역(dirty)과 통째로 옮겨진 영역(move) 목록.
// 화면 전체 대신 이 영역만 다시 그리거나 인코딩하려는 소비자를 위한 것이다.
//
// 두 목록은 한 버퍼에 이어서 담긴다 — 앞쪽이 move, 그 뒤가 dirty 다.
// 버퍼는 모자랄 때만 키워서 재사용한다. 프레임마다 새로 잡으면 60fps 에서
// 초당 60번의 할당이 된다.
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
		// 버퍼가 모자라면 키운다
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

		// 옮겨진 영역(move) — 버퍼 앞쪽에 담긴다
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

		// 바뀐 영역(dirty) — move 뒤에 이어서 담긴다
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

// =============================================================================
// 프레임 풀 발행 / 수신
//
// 캡처 스레드가 CopyCaptureTextureToPool 로 발행하고,
// 소비자 스레드가 GetLatestFrameHandle / ReleaseLatestFrameHandle 로 가져간다.
// =============================================================================

// 쓸 수 있는 슬롯을 찾아 복사하고 최신 프레임으로 발행한다.
//
// 마지막으로 쓴 슬롯의 다음 칸부터 찾는다. 방금 발행한 슬롯을 소비자가
// 가져가려는 참일 가능성이 가장 높기 때문이다.
//
// 슬롯을 쓸 수 있으려면 referenceCount 가 0 이고 BUSY 가 아니어야 한다.
// 상태를 CAS 로 선점한 뒤 referenceCount 를 한 번 더 확인하는데, 그 사이에
// 소비자가 이 슬롯을 집어 갔을 수 있어서다.
//
// 네 슬롯 모두 막혀 있으면 이번 프레임은 버린다. 기다리면 캡처 주기가
// 통째로 밀리고, 그 대가로 지키는 것은 어차피 최신이 아닌 프레임이다.
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

		const LONG referenceCount = ::ReadAcquire(&frameSlot.referenceCount);
		if (referenceCount != 0)
			continue;

		const LONG previousStatus = ::ReadAcquire(&frameSlot.status);
		if (previousStatus == FrameStatus::BUSY)
			continue;

		if (::InterlockedCompareExchange(&frameSlot.status, FrameStatus::BUSY, previousStatus) != previousStatus)
			continue;

		if (::ReadAcquire(&frameSlot.referenceCount) != 0)
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

			// 공유 풀이면 이 텍스처를 만지기 전에 뮤텍스를 잡아야 한다.
			// 우리가 만든 텍스처지만 예외가 아니다 — 한쪽만 지키면
			// 지키지 않는 것과 같다.
			//
			// 키는 0 하나만 쓴다. 생산자/소비자 키를 핑퐁시키는 방식은
			// "소비자가 손도 안 댄 채 버려지는 프레임" 에서 키가 한쪽에
			// 걸린 채 남아 그 슬롯을 영영 못 쓰게 만든다. 이 풀은
			// latest-only 로 소비되므로 그 상황이 정상 경로다.
			// 논리적 소유권은 status / referenceCount 가 이미 처리하고,
			// 뮤텍스는 디바이스 간 GPU 동기화만 맡으면 된다.
			//
			// 타임아웃은 유한해야 한다. INFINITE 로 두면 방금 걷어낸
			// 교착을 다른 이름으로 다시 만드는 것이다.
			if (frameSlot.keyedMutex)
			{
				const HRESULT acquireResult = frameSlot.keyedMutex->AcquireSync(
					FRAME_POOL_MUTEX_KEY, FRAME_POOL_MUTEX_TIMEOUT_MS);
				if (acquireResult != S_OK)
				{
					// 소비자가 아직 이 슬롯을 놓지 않았다. 이 프레임은
					// 버리고 다음 슬롯을 본다.
					::InterlockedExchange(&frameSlot.status, previousStatus);

					continue;
				}
			}

			context->CopyResource(frameSlot.texture, capturedTexture);
			if (frameSlot.copyDoneQuery)
			{
				context->End(frameSlot.copyDoneQuery);
			}
			else
			{
				// 쿼리가 없으면 GetData 도 없고, 명령 버퍼가 저절로 제출되지
				// 않는다. duplication 프레임을 놓기 전에 복사를 제출하되
				// 캡처 스레드는 막지 않아야 하므로 Flush 만 한다.
				context->Flush();
			}

			if (frameSlot.keyedMutex)
			{
				frameSlot.keyedMutex->ReleaseSync(FRAME_POOL_MUTEX_KEY);
			}
		}

		// 발행 순서가 중요하다. 슬롯을 먼저 완성(frameId -> READY)한 뒤에
		// 최신 표식을 옮긴다. 반대로 하면 소비자가 아직 채워지지 않은
		// 슬롯을 최신이라고 믿고 집어 간다.
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

// GPU 복사가 끝날 때까지 기다린다. 쿼리가 없으면(기능을 껐으면) 그냥 통과한다.
//
// Sleep(0) 으로 도는 이유: 복사 한 장은 보통 1ms 도 걸리지 않아서, 커널
// 대기로 넘어갔다 돌아오는 비용이 대기 자체보다 크다. 대신 게이트는 매
// 확인마다 놓는다 — 쥔 채로 돌면 그동안 캡처 스레드가 멈춘다.
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

// 최신 프레임을 참조로 잡아 돌려준다. 빈 핸들이면 새 프레임이 없는 것이다.
//
// 캡처 스레드를 막지 않으려고 락 대신 "잡고 나서 다시 확인" 하는 방식을 쓴다.
// referenceCount 를 먼저 올려 슬롯 재사용을 막은 뒤, 그 사이에 캡처가 이
// 슬롯을 덮어쓰지 않았는지 status/frameId/최신 표식을 다시 읽어 확인한다.
// 어긋나 있으면 참조를 놓고 다음 시도로 넘어간다.
//
// POOL_COUNT 번까지만 시도하는 이유: 그보다 자주 밀린다면 소비자가 캡처를
// 따라가지 못하는 것이고, 그건 여기서 더 기다려 해결할 문제가 아니다.
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
		const LONG status = ::ReadAcquire(&frameSlot.status);
		const LONG64 slotFrameId = ::ReadAcquire64(&frameSlot.frameId);

		if (status != FrameStatus::READY || slotFrameId != latestFrameId || !frameSlot.texture)
			continue;

		::InterlockedIncrement(&frameSlot.referenceCount);

		const LONG statusAfter = ::ReadAcquire(&frameSlot.status);
		const LONG64 slotFrameIdAfter = ::ReadAcquire64(&frameSlot.frameId);
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

// 잘못된 반납은 디버거를 붙일 수 없는 서버에서도 일어난다. 
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

LONG64 D3D11DuplicateEngine::GetLatestFrameID()
{
	return ::ReadAcquire64(&m_latestFrameId);
}

LONG D3D11DuplicateEngine::GetLatestFrameSlotID()
{
	return ::ReadAcquire(&m_latestFrameSlotId);
}

// =============================================================================
// 상태 / 이벤트 통지
// =============================================================================

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
	const CaptureEventCallback callback = m_eventCallback;
	void* const userData = m_eventUserData;

	if (callback)
	{
		callback(code, hr, userData);
	}
}

// =============================================================================
// 장애 복구
// =============================================================================

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

	// 이미 재연결 중이면 위에서 돌아갔으므로, 이 카운터는 "재연결에 들어간
	// 횟수" 이지 실패 횟수가 아니다.
	::InterlockedIncrement64(&m_accessLostCount);

	m_reconnectAttempt = 0;
	m_reconnectDelay_ms = kReconnectMinDelay_ms;

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
	// 대기 시간을 2배씩 늘리며 최대 kReconnectMaxDelay_ms 까지 Sleep 하도록 한다.
	// 이때, Sleep 은 무한정 하지 않고 만약 캡쳐 스레드 StopEvent 가 들어오면 바로 Sleep 에서 깨어난다.
	SleepUnlessStopping(m_reconnectDelay_ms);

	m_reconnectDelay_ms = (m_reconnectDelay_ms * 2 < kReconnectMaxDelay_ms)
		? m_reconnectDelay_ms * 2
		: kReconnectMaxDelay_ms;
}

// 정지 요청이 오면 즉시 깨어나는 대기. 캡처 스레드 전용이다.
//
// 그냥 Sleep 을 쓰면 재연결 백오프(최대 500ms)나 Faulted 유휴(50ms) 도중에는
// StopThread 가 그만큼 붙잡힌다. 정지 이벤트는 수동 리셋이라 한 번 신호되면
// 계속 신호 상태로 남고, 이후 대기는 모두 즉시 통과한다.
//
// 반환값은 "대기를 끝까지 채웠는가" 다. false 면 정지 요청이 온 것이므로
// 호출자는 하던 일을 접고 나가야 한다.
bool D3D11DuplicateEngine::SleepUnlessStopping(uint32_t milliseconds)
{
	HANDLE stopEvent = m_duplicateThread ? m_duplicateThread->GetStopEvent() : nullptr;
	if (!stopEvent)
	{
		// 스레드 없이 ProcessCaptureFrame 을 직접 돌리는 구성.
		::Sleep(milliseconds);
		return true;
	}

	return ::WaitForSingleObject(stopEvent, milliseconds) == WAIT_TIMEOUT;
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
	m_reconnectDelay_ms = kReconnectMinDelay_ms;
	::InterlockedIncrement64(&m_reconnectCount);

	SetCaptureState(CaptureState::Running);
	NotifyEvent(CaptureEventCode::Reconnected, S_OK);
}

// =============================================================================
// 통계 / 진단
// =============================================================================

// 각 카운터의 의미는 헤더의 멤버 선언부에 적어 두었다.
// 한 번에 모아 읽지만 스냅샷은 아니다 — 읽는 사이에도 캡처 스레드가 올린다.
// 카운터 사이의 정확한 비율이 필요한 계산에는 쓰지 않는다.
CaptureStats D3D11DuplicateEngine::GetStats() const
{
	const auto read64 = [](const volatile LONG64& value) -> uint64_t
	{
		return static_cast<uint64_t>(::ReadAcquire64(&value));
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
	stats.lastError = static_cast<HRESULT>(::ReadAcquire(&m_lastError));

	return stats;
}

// 구간별로 측정하고 싶을 때 호출자가 직접 0 으로 되돌린다.
// 캡처가 도는 중에 불러도 되지만 그 순간의 증가분 몇 개는 유실될 수 있다.
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

void D3D11DuplicateEngine::DebugSimulateAccessLoss()
{
	// 캡처 스레드가 직접 처리하게 둔다. duplication 객체를 다른 스레드에서
	// 만지면 그 자체가 경쟁 상태가 된다.
	::InterlockedExchange(&m_debugForceAccessLoss, TRUE);
}

// =============================================================================
// getter / setter
// =============================================================================

// --- 런타임 설정 ---

void D3D11DuplicateEngine::SetSkipUnchangedFrames(bool enabled)
{
	::InterlockedExchange(&m_skipUnchangedFrames, enabled ? TRUE : FALSE);
}

bool D3D11DuplicateEngine::IsSkipUnchangedFramesEnabled() const
{
	return ::ReadAcquire(&m_skipUnchangedFrames) != FALSE;
}

void D3D11DuplicateEngine::SetTargetFps(uint64_t fps)
{
	::WriteRelease64(&m_captureFPS, static_cast<LONG64>(fps));
}

uint64_t D3D11DuplicateEngine::GetTargetFps() const
{
	return static_cast<uint64_t>(::ReadAcquire64(&m_captureFPS));
}

// --- 상태 조회 ---

CaptureState D3D11DuplicateEngine::GetCaptureState() const
{
	return static_cast<CaptureState>(::ReadAcquire(&m_captureState));
}

uint64_t D3D11DuplicateEngine::GetDroppedFrameCount()
{
	return static_cast<uint64_t>(::ReadAcquire64(&m_droppedFrameCount));
}

// --- 설정 조회 ---

bool D3D11DuplicateEngine::IsImmediateContextGateEnabled() const
{
	return m_D3D11Engine
		? m_D3D11Engine->IsImmediateContextGateEnabled()
		: m_immediateContextGateEnabled;
}

// --- 출력 / 디바이스 조회 ---

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

ID3D11Device1* D3D11DuplicateEngine::GetD3DDevice()
{
	if (!m_D3D11Engine)
	{
		return nullptr;
	}

	return m_D3D11Engine->GetD3DDevice();
}
