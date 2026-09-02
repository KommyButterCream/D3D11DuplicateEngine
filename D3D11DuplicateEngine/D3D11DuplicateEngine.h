#pragma once

#ifdef BUILD_D3D11_DUPLICATE_ENGINE
#define D3D11_DUPLICATE_ENGINE_API __declspec(dllexport)
#else
#define D3D11_DUPLICATE_ENGINE_API __declspec(dllimport)
#endif

#include <stdint.h>
#include "CommonTypes.h"

class D3D11RenderEngine;
class D3D11DuplicateThread;
struct IDXGIOutput1;
struct IDXGIOutputDuplication;
struct IDXGIKeyedMutex;

using FrameCallback = void(*)(void* userData);

// 캐시 라인 정렬(alignas(64)) 때문에 구조체 끝에 패딩이 붙는다. 의도한 것이므로
// 이 헤더를 /W4 로 가져다 쓰는 쪽에 경고가 새어 나가지 않게 막는다.
#pragma warning(push)
#pragma warning(disable: 4324)

class D3D11_DUPLICATE_ENGINE_API D3D11DuplicateEngine
{
public:
	D3D11DuplicateEngine() = default;
	~D3D11DuplicateEngine();

	bool Initialize(D3D11RenderEngine* D3D11Engine = nullptr, uint32_t outputIndex = 0);
	bool IsInitialized() const { return m_initialized; }
	void Shutdown();
	bool SetCaptureOutputMode(CaptureOutputMode outputMode);
	CaptureOutputMode GetCaptureOutputMode() const { return m_captureOutputMode; }
	bool SetImmediateContextGateEnabled(bool enabled);
	bool IsImmediateContextGateEnabled() const;
	bool SetWaitForFrameCopyCompletion(bool enabled);
	bool IsWaitForFrameCopyCompletionEnabled() const { return m_waitForFrameCopyCompletion; }

	// 데스크톱 이미지가 갱신되지 않은 프레임(마우스만 움직인 경우)의 복사와
	// 발행을 건너뛴다. 기본값 켜짐. 정지 화면에서 인코딩 부하가 사라진다.
	//
	// duplication 은 포인터만 움직여도 프레임을 돌려주는데, 그때
	// frameInfo.LastPresentTime 이 0 이고 화면 내용은 직전과 동일하다.
	// 소비자가 포인터 갱신마다 프레임을 받아야 하는 경우에만 끈다.
	//
	// 언제든 바꿀 수 있다.
	void SetSkipUnchangedFrames(bool enabled);
	bool IsSkipUnchangedFramesEnabled() const;

	void SetTargetFps(uint64_t fps);
	uint64_t GetTargetFps() const;

	uint32_t GetOutputCount() const;
	uint32_t GetOutputWidth();
	uint32_t GetOutputHeight();

	// Capture Thread
	bool StartThread();
	void StopThread();

	void SetFrameCaptureCallback(FrameCallback funcCallback, void* userData); // 스레드에서 호출 할 함수 등록

	// 접근 상실 / 디바이스 상실 / 재연결 같은 사건을 통지받는다.
	// 캡처 스레드에서 불리므로 블로킹 작업을 하면 안 된다.
	void SetCaptureEventCallback(CaptureEventCallback funcCallback, void* userData);

	CaptureState GetCaptureState() const;
	bool IsFaulted() const { return GetCaptureState() == CaptureState::Faulted; }

	CaptureStats GetStats() const;
	void ResetStats();

	// 테스트 전용 훅. 다음 캡처 반복에서 duplication 접근 상실이 일어난 것처럼
	// 만들어 재연결 경로를 강제로 태운다. 운영 코드에서 호출하지 않는다.
	void DebugSimulateAccessLoss();

	CapturedFrameHandle GetLatestFrameHandle();
	void ReleaseLatestFrameHandle(CapturedFrameHandle& handle);
	uint64_t GetDroppedFrameCount();

	ID3D11Device1* GetD3DDevice();
	HANDLE GetSharedTextureHandle() const;

private:
	friend class D3D11DuplicateThread;

	HRESULT InitializeDuplication(uint32_t outputIndex);
	HRESULT CreateSharedTexture(UINT width, UINT height, ID3D11Texture2D** texture, HANDLE* sharedHandle);

	bool InitializeCaptureFramePool();
	void DestroyCaptureFramePool();
	void CopyCaptureTextureToPool(ID3D11Texture2D* capturedTexture, const DXGI_OUTDUPL_FRAME_INFO& frameInfo, const PTR_INFO& mouseInfo);
	LONG64 GetLatestFrameID();
	LONG GetLatestFrameSlotID();
	bool WaitForFrameSlotCopy(CapturedFrameSlot& frameSlot);

	bool UpdateMouseInfo(DXGI_OUTDUPL_FRAME_INFO& frameInfo);
	bool UpdateDirtyMoveInfo(DXGI_OUTDUPL_FRAME_INFO& frameInfo, CaptureFrameResult& outResult);

	void SetCaptureState(CaptureState state);
	void NotifyEvent(CaptureEventCode code, HRESULT hr);
	void RecordError(HRESULT hr);

	// 출력 리소스(프레임 풀 또는 공유 텍스처)를 현재 m_duplDesc 기준으로 만든다.
	bool CreateFrameResources();
	void DestroyFrameResources();

	// 정지 요청이 오면 즉시 깨어나는 대기. 캡처 스레드 전용.
	// false 를 돌려주면 정지 요청이 온 것이다.
	bool SleepUnlessStopping(uint32_t milliseconds);

	// 복구
	bool IsDeviceLost() const;
	void EnterReconnecting(HRESULT hr);
	void EnterFaulted(CaptureEventCode code, HRESULT hr);
	void BackoffReconnectDelay();
	bool RecreateLostDevice();
	void RecoverDuplication();

	// Capture Thread
	void ProcessCaptureFrame();

	bool AcquireFrame(UINT timeout_ms, CaptureFrameResult& outResult);
	void ReleaseFrame();

private:
	bool m_initialized = false;
	uint32_t m_outputIndex = 0;
	bool m_useMouseInfo = false;
	bool m_useMoveDiryInfo = false;

	// Render Engine
	D3D11RenderEngine* m_D3D11Engine = nullptr;
	bool m_ownsD3D11Engine = false;
	bool m_immediateContextGateEnabled = false;
	bool m_immediateContextGateSettingExplicit = false;

	IDXGIOutput1* m_dxgiOutput = nullptr;
	IDXGIOutputDuplication* m_deskDupl = nullptr;

	DXGI_OUTDUPL_DESC m_duplDesc = {};
	DXGI_OUTPUT_DESC m_outputDesc = {};

	// Capture Frame per second
	// 페이싱 스레드가 매 반복 읽고 임의 스레드가 SetTargetFps 로 쓴다.
	volatile LONG64 m_captureFPS = 0;

	// Capture Result
	static constexpr size_t POOL_COUNT = 4;
	CapturedFrameSlot m_framePool[POOL_COUNT];
	static_assert((POOL_COUNT& (POOL_COUNT - 1)) == 0, "Pool count must be power of two");
	alignas(64) volatile LONG64 m_latestFrameId = 0;
	alignas(64) volatile LONG m_latestFrameSlotId = -1;
	alignas(64) volatile LONG64 m_droppedFrameCount = 0;
	// 잘못된 핸들 반납 횟수. 예전에는 __debugbreak 로 세웠던 자리다.
	alignas(64) volatile LONG64 m_invalidReleaseCount = 0;

	// 화면 갱신이 없는 프레임을 건너뛸지. 캡처 스레드가 매 프레임 읽으므로 원자적.
	volatile LONG m_skipUnchangedFrames = TRUE;

	// Stats / State
	alignas(64) volatile LONG64 m_capturedFrameCount = 0;
	volatile LONG64 m_skippedFrameCount = 0;
	volatile LONG64 m_timeoutCount = 0;
	volatile LONG64 m_accessLostCount = 0;
	volatile LONG64 m_reconnectCount = 0;
	volatile LONG64 m_deviceRecreateCount = 0;
	volatile LONG m_lastError = S_OK;
	alignas(64) volatile LONG m_captureState = static_cast<LONG>(CaptureState::Idle);

	// 복구. 전부 캡처 스레드 전용이라 원자성이 필요 없다.
	uint32_t m_reconnectAttempt = 0;
	uint32_t m_reconnectDelayMs = 0;
	bool m_deviceRemovedNotified = false;
	volatile LONG m_debugForceAccessLoss = FALSE;

	// 현재 출력 리소스가 맞춰진 크기. 재연결 후 해상도 변경 감지에 쓴다.
	uint32_t m_frameWidth = 0;
	uint32_t m_frameHeight = 0;


	// Capture Image
	CaptureOutputMode m_captureOutputMode = CaptureOutputMode::FramePool;
	bool m_waitForFrameCopyCompletion = true;
	HANDLE m_sharedHandle = nullptr;
	ID3D11Texture2D* m_capturedTexture = nullptr; // 현재 잡고 있는 프레임
	ID3D11Texture2D* m_sharedTexture = nullptr; // 로컬 공유용 버퍼
	IDXGIKeyedMutex* m_sharedKeyedMutex = nullptr;
	bool m_frameAcquired = false;

	BYTE* m_metaDataBuffer = nullptr;
	UINT m_metaDataSize = 0;
	PTR_INFO m_mouseInfo = {};          // 최신 마우스 상태

	// Capture Thread
	D3D11DuplicateThread* m_duplicateThread = nullptr;

	FrameCallback m_frameCallback = nullptr;
	void* m_userData = nullptr;

	CaptureEventCallback m_eventCallback = nullptr;
	void* m_eventUserData = nullptr;
};

#pragma warning(pop)

