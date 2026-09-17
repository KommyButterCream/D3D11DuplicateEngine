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

using FrameCallback = void(*)(void* userData);

// 캐시 라인 정렬(alignas(64)) 때문에 구조체 끝에 패딩이 붙는다. 의도한 것이므로
// 이 헤더를 /W4 로 가져다 쓰는 쪽에 경고가 새어 나가지 않게 막는다.
#pragma warning(push)
#pragma warning(disable: 4324)

// 사용 순서
//   1) 초기화 전 설정 (SetFramePoolSharable / SetWaitForFrameCopyCompletion / ...)
//   2) Initialize
//   3) 콜백 등록 -> StartThread
//   4) 콜백에서 GetLatestFrameHandle / ReleaseLatestFrameHandle
//   5) StopThread -> Shutdown
class D3D11_DUPLICATE_ENGINE_API D3D11DuplicateEngine
{
public:
	// =====================================================================
	// 생성 / 소멸
	// =====================================================================
	D3D11DuplicateEngine() = default;
	~D3D11DuplicateEngine();

	// =====================================================================
	// 초기화 전 설정
	//
	// 아래 설정들은 Initialize 가 만들어 낼 리소스의 모양을 결정한다.
	// 이미 초기화된 뒤에 바꾸면 만들어진 리소스와 설정이 어긋나므로
	// 전부 false 를 돌려주고 거절한다.
	// =====================================================================
	// 렌더 엔진의 immediate context 를 여럿이 나눠 쓸 때 켠다.
	// 외부 엔진을 받는 경우, 명시하지 않으면 그 엔진의 설정을 그대로 따른다.
	bool SetImmediateContextGateEnabled(bool enabled);

	// 슬롯마다 EVENT 쿼리를 만들어, 소비자가 프레임을 가져갈 때 GPU 복사가
	// 실제로 끝났는지 확인하게 한다. 기본값 켜짐.
	// 끄면 한 번의 대기가 사라지지만 복사 중인 텍스처를 읽을 수 있다.
	bool SetWaitForFrameCopyCompletion(bool enabled);

	// --- 프레임 풀 공유 (다른 D3D11 디바이스에서 열어 쓰기) ---
	//
	// 왜 필요한가
	//   캡처와 인코딩이 같은 immediate context 를 쓰면 하나의 게이트를
	//   공유하게 되고, 인코더가 그 게이트를 쥔 채 드라이버 안에서
	//   블로킹하면 캡처가 통째로 멈춘다. 실제로 QHD 60fps 에서 그렇게
	//   교착했다.
	//
	//   디바이스를 나누면 서로의 컨텍스트가 독립이라 그 순환이 성립하지
	//   않는다. 대신 캡처 결과를 건네줄 길이 필요하고, 그게 이 공유 풀이다.
	//   (OBS 가 인코더에 별도 디바이스를 주고 쓰는 것과 같은 구조다)
	//
	// 사용법
	//   Initialize 전에 SetFramePoolSharable(true).
	//   Initialize 후에 슬롯마다 GetFramePoolSharedHandle() 로 핸들을 받아
	//   소비자 디바이스에서 OpenSharedResource1 로 한 번만 열어 둔다.
	//   매 프레임 여는 것이 아니다 — 그건 또 하나의 드라이버 왕복이다.
	//
	//   이후 소비자는 GetLatestFrameHandle() 이 준 slotId 로 자기가 미리
	//   열어 둔 텍스처를 찾고, 쓰기 전에 그 텍스처의 IDXGIKeyedMutex 를
	//   AcquireSync 해야 한다. 키는 0 하나만 쓴다(CommonTypes.h 참고).
	//
	// 기본값은 꺼짐이다. 켜면 텍스처가 KEYEDMUTEX 로 만들어지므로 이
	// 텍스처를 만지는 모든 주체가 예외 없이 뮤텍스를 잡아야 한다.
	// 기존 소비자를 조용히 깨뜨리지 않으려고 opt-in 으로 둔다.
	bool SetFramePoolSharable(bool enabled);

	// =====================================================================
	// 초기화 / 종료
	// =====================================================================
	bool Initialize(D3D11RenderEngine* D3D11Engine = nullptr, uint32_t outputIndex = 0);
	void Shutdown();

	// =====================================================================
	// 프레임 풀 공유 핸들 (소비자 디바이스 연결)
	//
	// Initialize 이후에만 유효하다. 슬롯 수만큼 한 번씩 열어 두고 이후에는
	// GetLatestFrameHandle() 이 주는 slotId 로 찾아 쓴다.
	// =====================================================================
	uint32_t GetFramePoolCount() const;
	HANDLE GetFramePoolSharedHandle(uint32_t slot) const;

	// =====================================================================
	// 캡처 스레드 제어
	// =====================================================================
	bool StartThread();
	void StopThread();

	// =====================================================================
	// 콜백 등록
	//
	// 둘 다 캡처 스레드에서 불린다. 블로킹 작업을 하면 캡처 주기가 그대로
	// 밀리므로 하면 안 된다.
	// =====================================================================
	void SetFrameCaptureCallback(FrameCallback funcCallback, void* userData);

	// 접근 상실 / 디바이스 상실 / 재연결 같은 사건을 통지받는다.
	void SetCaptureEventCallback(CaptureEventCallback funcCallback, void* userData);

	// =====================================================================
	// 프레임 수신
	//
	// 받은 핸들은 반드시 ReleaseLatestFrameHandle 로 돌려줘야 한다.
	// 돌려주지 않으면 그 슬롯이 풀에서 영구히 빠진다.
	// =====================================================================
	CapturedFrameHandle GetLatestFrameHandle();
	void ReleaseLatestFrameHandle(CapturedFrameHandle& handle);

	// =====================================================================
	// 통계 / 진단
	// =====================================================================
	CaptureStats GetStats() const;
	void ResetStats();

	// 테스트 전용 훅. 다음 캡처 반복에서 duplication 접근 상실이 일어난 것처럼
	// 만들어 재연결 경로를 강제로 태운다. 운영 코드에서 호출하지 않는다.
	void DebugSimulateAccessLoss();

	// =====================================================================
	// 런타임 설정 (언제든 바꿀 수 있다)
	// =====================================================================

	// 데스크톱 이미지가 갱신되지 않은 프레임(마우스만 움직인 경우)의 복사와
	// 발행을 건너뛴다. 기본값 켜짐. 정지 화면에서 인코딩 부하가 사라진다.
	//
	// duplication 은 포인터만 움직여도 프레임을 돌려주는데, 그때
	// frameInfo.LastPresentTime 이 0 이고 화면 내용은 직전과 동일하다.
	// 소비자가 포인터 갱신마다 프레임을 받아야 하는 경우에만 끈다.
	void SetSkipUnchangedFrames(bool enabled);
	bool IsSkipUnchangedFramesEnabled() const;

	// 캡처 스레드의 목표 주기. 0 은 대기 없는 무제한 캡처다.
	// 이 값만으로는 아무 일도 일어나지 않는다 — 실제 페이싱은 캡처 스레드가 한다.
	void SetTargetFps(uint64_t fps);
	uint64_t GetTargetFps() const;

	// =====================================================================
	// 상태 조회
	// =====================================================================
	bool IsInitialized() const { return m_initialized; }
	CaptureState GetCaptureState() const;
	bool IsFaulted() const { return GetCaptureState() == CaptureState::Faulted; }
	uint64_t GetDroppedFrameCount();

	// =====================================================================
	// 설정 조회
	// =====================================================================
	bool IsImmediateContextGateEnabled() const;
	bool IsWaitForFrameCopyCompletionEnabled() const { return m_waitForFrameCopyCompletion; }
	bool IsFramePoolSharable() const { return m_framePoolSharable; }

	// =====================================================================
	// 출력 / 디바이스 조회
	// =====================================================================
	// 연결된 모니터 수. Initialize 전에도 부를 수 있다(outputIndex 선택용).
	uint32_t GetOutputCount() const;

	// 캡처 중인 해상도. 재연결로 해상도가 바뀌면 이 값도 따라 바뀌므로
	// 소비자는 ModeChanged 통지를 받은 뒤 다시 읽어야 한다.
	uint32_t GetOutputWidth();
	uint32_t GetOutputHeight();

	ID3D11Device1* GetD3DDevice();

private:
	friend class D3D11DuplicateThread;

	// --- 초기화 / 리소스 생성 ---

	// 어댑터 -> 모니터 -> duplication 순으로 연다. 재연결에서도 이 경로를 탄다.
	HRESULT InitializeDuplication(uint32_t outputIndex);

	// 프레임 풀을 현재 m_duplDesc 기준으로 만든다.
	// 해상도가 바뀌면 기존 텍스처는 쓸 수 없으므로 재연결 후에도 다시 부른다.
	bool CreateFrameResources();
	void DestroyFrameResources();

	// FramePool 모드용 슬롯 텍스처 4장. 공유 설정이면 슬롯마다 NT 공유 핸들과
	// 키드 뮤텍스까지 여기서 한 번만 만들어 둔다.
	bool InitializeCaptureFramePool();
	void DestroyCaptureFramePool();

	// --- 캡처 루프 (캡처 스레드 전용) ---

	// 캡처 한 반복. 아래 함수들은 전부 이 안에서만 불린다.
	void ProcessCaptureFrame();

	// duplication 에서 한 프레임을 받아 온다. 실패 처리와 재연결 진입까지
	// 이 안에서 끝내므로 호출자는 반환값만 보면 된다.
	bool AcquireFrame(UINT timeout_ms, CaptureFrameResult& outResult);
	// 받은 프레임을 duplication 에 돌려준다. 돌려주기 전에는 다음 프레임을 받을 수 없다.
	void ReleaseFrame();

	// duplication 은 커서의 변경분만 주므로 m_mouseInfo 에 누적한다.
	bool UpdateMouseInfo(DXGI_OUTDUPL_FRAME_INFO& frameInfo);
	// 이번 프레임에서 바뀐 영역(dirty)과 옮겨진 영역(move) 목록을 받아 온다.
	bool UpdateDirtyMoveInfo(DXGI_OUTDUPL_FRAME_INFO& frameInfo, CaptureFrameResult& outResult);

	// --- 프레임 풀 발행 ---

	// 비어 있는 슬롯을 찾아 복사하고 최신 프레임으로 발행한다.
	// 쓸 수 있는 슬롯이 하나도 없으면 이번 프레임을 버린다(m_droppedFrameCount).
	void CopyCaptureTextureToPool(ID3D11Texture2D* capturedTexture, const DXGI_OUTDUPL_FRAME_INFO& frameInfo, const PTR_INFO& mouseInfo);
	// 소비자가 텍스처를 읽기 전에 GPU 복사 완료를 확인한다.
	// m_waitForFrameCopyCompletion 이 꺼져 있으면 쿼리가 없어 그냥 통과한다.
	bool WaitForFrameSlotCopy(CapturedFrameSlot& frameSlot);

	LONG64 GetLatestFrameID();
	LONG GetLatestFrameSlotID();

	// --- 상태 / 이벤트 통지 ---
	void SetCaptureState(CaptureState state);
	// 등록된 콜백으로 사건을 알린다. 콜백이 없으면 조용히 지나간다.
	void NotifyEvent(CaptureEventCode code, HRESULT hr);
	// 진단용으로 마지막 HRESULT 만 남긴다. 제어 흐름은 바꾸지 않는다.
	void RecordError(HRESULT hr);

	// --- 장애 복구 ---

	// 디바이스가 제거됐는가. 엔진이 없으면 잃은 것으로 친다.
	bool IsDeviceLost() const;
	// 재연결 상태로 전환. 여기서 duplication 을 놓고 백오프를 초기화한다.
	void EnterReconnecting(HRESULT hr);
	// 스스로 복구할 수 없는 상태. 호출자가 Shutdown/Initialize 로 되살려야 한다.
	void EnterFaulted(CaptureEventCode code, HRESULT hr);
	// 대기 후 다음 대기 시간을 2배로 늘린다(최대 500ms).
	void BackoffReconnectDelay();
	// 우리가 만든 엔진일 때만 디바이스를 다시 만든다.
	// 외부 엔진은 다른 사용자의 리소스까지 깨뜨리므로 Faulted 로 넘긴다.
	bool RecreateLostDevice();
	// 재연결 한 번의 시도. 실패하면 백오프 후 돌아와 정지 요청에 빠르게 반응한다.
	void RecoverDuplication();

	// 정지 요청이 오면 즉시 깨어나는 대기. 캡처 스레드 전용.
	// false 를 돌려주면 정지 요청이 온 것이다.
	bool SleepUnlessStopping(uint32_t milliseconds);

private:
	// =====================================================================
	// 초기화 시점에 정해지고 이후 바뀌지 않는 것들
	//
	// 스레드가 돌기 전에 쓰이고 그 뒤로는 읽기만 하므로 원자성이 필요 없다.
	// =====================================================================
	// Initialize 성공부터 Shutdown 까지 true. 초기화 전 설정들의 거절 기준.
	bool m_initialized = false;

	// 마우스 커서 위치/모양을 프레임과 함께 실어 보낼지.
	// dirty/move 사각형 메타데이터를 수집할지.
	// 둘 다 현재 켜는 경로가 없다(설정 API 미노출). 관련 코드는 남겨 둔다.
	bool m_useMouseInfo = false;
	bool m_useMoveDiryInfo = false;

	// 엔진을 우리가 만들었는가. 디바이스가 사라졌을 때 되살릴 수 있는지를
	// 가른다 — 외부 엔진은 다른 사용자와 공유되므로 우리가 다시 만들 수 없다.
	bool m_ownsD3D11Engine = false;

	bool m_immediateContextGateEnabled = false;
	// 호출자가 게이트 설정을 명시했는가. 외부 엔진의 기존 설정을 덮어쓸지
	// 판단하는 데만 쓴다. 명시하지 않았으면 외부 엔진의 설정을 따른다.
	bool m_immediateContextGateSettingExplicit = false;

	// 슬롯마다 EVENT 쿼리를 만들어 소비자가 GPU 복사 완료를 확인하게 할지.
	// 끄면 지연은 줄지만 아직 복사 중인 텍스처를 읽을 수 있다.
	bool m_waitForFrameCopyCompletion = true;

	// 프레임 풀 텍스처를 KEYEDMUTEX 공유로 만들지. Initialize 전에만 바뀐다.
	bool m_framePoolSharable = false;

	// 캡처 대상 모니터 인덱스. 재연결이 이 값으로 duplication 을 다시 열므로
	// duplication 을 열기 전에 확정돼야 한다.
	uint32_t m_outputIndex = 0;

	// =====================================================================
	// D3D / DXGI 객체
	// =====================================================================

	// 외부 주입이거나 우리가 만든 것. 어느 쪽인지는 m_ownsD3D11Engine 이 안다.
	D3D11RenderEngine* m_D3D11Engine = nullptr;

	IDXGIOutput1* m_dxgiOutput = nullptr;
	// duplication 본체. ACCESS_LOST 마다 놓고 다시 연다.
	IDXGIOutputDuplication* m_deskDupl = nullptr;

	// 캡처 해상도. 풀 텍스처 크기의 근거이자 해상도 변경 감지의 기준값.
	DXGI_OUTDUPL_DESC m_duplDesc = {};
	// 이 모니터가 데스크톱 좌표계에서 차지하는 영역.
	// 마우스 위치를 모니터 로컬에서 전역 좌표로 옮길 때 쓴다.
	DXGI_OUTPUT_DESC m_outputDesc = {};

	// =====================================================================
	// 캡처 출력 리소스
	// =====================================================================

	// 슬롯 4개인 이유: 소비자가 한 장을 쥐고 있고 캡처가 다음 한 장에 쓰는
	// 동안에도 여유가 남아야, 한 프레임 늦은 소비자 때문에 드롭이 나지 않는다.
	static constexpr size_t POOL_COUNT = 4;
	// 다음 슬롯을 & (POOL_COUNT - 1) 로 고르므로 2의 거듭제곱이어야 한다.
	static_assert((POOL_COUNT & (POOL_COUNT - 1)) == 0, "Pool count must be power of two");
	CapturedFrameSlot m_framePool[POOL_COUNT];

	// 현재 출력 리소스가 맞춰진 크기. 재연결 후 해상도 변경 감지에 쓴다.
	uint32_t m_frameWidth = 0;
	uint32_t m_frameHeight = 0;

	// =====================================================================
	// 런타임 설정 — 임의 스레드가 쓰고 캡처 스레드가 매 반복 읽는다.
	//
	// 읽기가 압도적으로 잦고 쓰기는 거의 없다. 캡처 스레드가 매 프레임
	// 갱신하는 아래 카운터들과 같은 라인에 두면 그 쓰기마다 이 라인이
	// 무효화되므로 떼어 둔다.
	// =====================================================================
	// 목표 캡처 주기. 페이싱은 이 엔진이 아니라 D3D11DuplicateThread 가 한다.
	// 0 은 무제한 캡처(대기 없음)를 뜻한다.
	alignas(64) volatile LONG64 m_captureFPS = 0;
	volatile LONG m_skipUnchangedFrames = TRUE;

	// =====================================================================
	// 최신 프레임 발행 — 캡처 스레드가 쓰고 소비자 스레드가 읽는다.
	//
	// 두 값은 CopyCaptureTextureToPool 에서 한 쌍으로 발행되고 소비자도
	// GetLatestFrameHandle 에서 한 쌍으로 읽는다. 같은 캐시 라인에 두는
	// 것이 맞다 — 나눠 두면 발행/소비 때마다 라인을 두 번 가져온다.
	// =====================================================================
	// 1 부터 단조 증가. 소비자는 이 값이 자기가 마지막에 받은 것과 같으면
	// 새 프레임이 없다고 판단한다. 슬롯 재사용 중 읽힌 낡은 데이터를
	// 걸러내는 표식이기도 하다.
	alignas(64) volatile LONG64 m_latestFrameId = 0;
	// 그 프레임이 들어 있는 슬롯. 아직 발행된 것이 없으면 -1.
	volatile LONG m_latestFrameSlotId = -1;

	// =====================================================================
	// 통계 카운터 — 사실상 캡처 스레드 전용 쓰기.
	//
	// 매 프레임 갱신되므로 위의 두 라인과는 반드시 떨어져 있어야 한다.
	// m_invalidReleaseCount 만 소비자 스레드가 쓰지만 잘못된 반납이라는
	// 오류 경로에서만 올라가므로 정상 운영에서 이 라인을 건드리지 않는다.
	// =====================================================================
	// 발행에 성공한 프레임 수. 복사와 콜백까지 끝난 것만 센다.
	// 경과 시간으로 나누면 실제 캡처 fps 다.
	alignas(64) volatile LONG64 m_capturedFrameCount = 0;

	// 화면 내용이 직전과 같아서 복사를 건너뛴 수(마우스만 움직인 경우).
	// 정지 화면에서는 이쪽이 대부분이며 정상이다.
	volatile LONG64 m_skippedFrameCount = 0;

	// 버린 프레임 수. 풀 슬롯 4개가 전부 사용 중이거나 공유 뮤텍스를
	// 제한 시간 안에 못 잡았을 때 오른다.
	// 꾸준히 오르면 소비자가 핸들을 제때 반납하지 않고 있다는 뜻이다.
	volatile LONG64 m_droppedFrameCount = 0;

	// AcquireNextFrame 이 대기 시간을 채우고 빈손으로 돌아온 수.
	// 화면이 전혀 바뀌지 않으면 계속 오른다. 오류가 아니다.
	volatile LONG64 m_timeoutCount = 0;

	// 재연결에 들어간 횟수. 잠금화면, UAC 보안 데스크톱, 해상도 변경,
	// 전체화면 전환, TDR, RDP 연결에서 일상적으로 오른다.
	volatile LONG64 m_accessLostCount = 0;

	// 재연결에 성공한 횟수. accessLost 와 벌어져 있으면 아직 복구되지
	// 못했거나 복구에 여러 번 실패하고 있다는 뜻이다.
	volatile LONG64 m_reconnectCount = 0;

	// D3D 디바이스를 새로 만든 횟수(TDR, 드라이버 교체 등).
	// 오르면 그 전에 나눠 준 텍스처와 공유 핸들이 전부 무효다.
	volatile LONG64 m_deviceRecreateCount = 0;

	// 잘못된 핸들 반납 횟수. 모르는 slotId 이거나 이중 반납일 때 오른다.
	// 예전에는 __debugbreak 로 세웠던 자리다. 0 이 아니면 소비자 쪽 버그다.
	volatile LONG64 m_invalidReleaseCount = 0;

	// =====================================================================
	// 상태 — 캡처 스레드가 드물게 쓰고 외부 스레드가 자주 읽는다.
	//
	// IsFaulted() 같은 폴링이 위 카운터 라인에 끼면 매 프레임 무효화된
	// 라인을 다시 가져오게 되므로 따로 둔다.
	// =====================================================================
	// Idle / Running / Reconnecting / Faulted. 캡처 루프의 분기 기준이다.
	alignas(64) volatile LONG m_captureState = static_cast<LONG>(CaptureState::Idle);
	// 마지막으로 기록된 HRESULT. 진단용이며 제어 판단에는 쓰지 않는다.
	volatile LONG m_lastError = S_OK;

	// =====================================================================
	// 캡처 스레드 전용 상태 — 다른 스레드가 건드리지 않으므로 원자성 불필요
	// =====================================================================
	// AcquireNextFrame 이 준 데스크톱 표면. ReleaseFrame 까지만 유효하다.
	ID3D11Texture2D* m_capturedTexture = nullptr;
	// duplication 프레임을 쥐고 있는가. 이중 ReleaseFrame 을 막는다.
	// 쥔 채로 재연결에 들어가면 그 프레임을 영영 놓을 수 없다.
	bool m_frameAcquired = false;

	// dirty/move 사각형 수신 버퍼. 프레임마다 새로 잡지 않고 모자랄 때만
	// 키워서 재사용한다. m_metaDataSize 는 그 버퍼의 현재 용량이다.
	BYTE* m_metaDataBuffer = nullptr;
	UINT m_metaDataSize = 0;

	// 최신 마우스 상태. duplication 이 변경분만 주므로 프레임마다 누적 갱신한다.
	PTR_INFO m_mouseInfo = {};

	// --- 복구 ---
	// 현재 재연결 시도의 연속 실패 횟수. 통지 간격(20회마다)을 재는 데 쓴다.
	uint32_t m_reconnectAttempt = 0;
	// 다음 백오프 대기 시간. 50ms 에서 시작해 500ms 까지 2배씩 늘어난다.
	uint32_t m_reconnectDelay_ms = 0;
	// DeviceRemoved 를 이미 통지했는가. 재시도마다 같은 통지가 반복되는 것을 막는다.
	bool m_deviceRemovedNotified = false;

	// 테스트 훅. 외부 스레드가 세우고 캡처 스레드가 소비한다.
	volatile LONG m_debugForceAccessLoss = FALSE;

	// =====================================================================
	// 캡처 스레드 / 콜백
	// =====================================================================
	// StartThread 에서 만들고 StopThread 에서 지운다.
	// 페이싱(목표 fps 대기)과 정지 이벤트를 이 스레드가 갖는다.
	D3D11DuplicateThread* m_duplicateThread = nullptr;

	// 프레임 발행 직후 캡처 스레드에서 호출된다.
	FrameCallback m_frameCallback = nullptr;
	void* m_userData = nullptr;

	// 접근 상실 / 재연결 / 디바이스 재생성 / 해상도 변경을 통지한다.
	CaptureEventCallback m_eventCallback = nullptr;
	void* m_eventUserData = nullptr;
};

#pragma warning(pop)
