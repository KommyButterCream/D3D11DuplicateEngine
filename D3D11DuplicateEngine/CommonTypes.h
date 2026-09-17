#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <stdint.h>

// 마우스 정보 (MS 예제 참고)
struct PTR_INFO
{
	BYTE* shapeBuffer = nullptr;
	DXGI_OUTDUPL_POINTER_SHAPE_INFO shapeInfo = {};
	POINT position = {};
	bool visible = false;
	UINT bufferSize = 0;
	UINT whoUpdatedPositionLast = 0;
	LARGE_INTEGER lastTimeStamp = {};
};

// 캡처된 프레임의 결과물
struct CaptureFrameResult
{
	ID3D11Texture2D* texture = nullptr;      // 캡처된 텍스처
	DXGI_OUTDUPL_FRAME_INFO frameInfo = {};   // 프레임 메타데이터 (Dirty Rects 등)
	BYTE* metaData = nullptr;
	UINT dirtyCount = 0;
	UINT moveCount = 0;
	PTR_INFO mouseInfo = {};   // 마우스 위치 및 모양

	// 데스크톱 이미지가 실제로 갱신되었는가(frameInfo.LastPresentTime != 0).
	// false 면 마우스만 움직인 것이고 화면 내용은 직전 프레임과 같다.
	bool desktopUpdated = false;
};

// 캡처 파이프라인의 진행 상태.
enum class CaptureState : uint32_t
{
	Idle = 0,       // 아직 초기화되지 않음
	Running,        // 정상 캡처 중
	Reconnecting,   // duplication 재연결 시도 중
	Faulted,        // 복구 불가. Shutdown 후 Initialize 가 필요하다.
};

// 상위에 알리는 사건. AcquireNextFrame 타임아웃처럼 정상 범주의
// 일시적 상황은 통지하지 않는다.
enum class CaptureEventCode : uint32_t
{
	None = 0,
	AccessLost,        // duplication 접근 상실. 엔진이 자동 재연결에 들어간다.
	Reconnecting,      // 재연결이 길어지는 중(주기적으로 한 번씩만 통지)
	Reconnected,       // 재연결 성공. 캡처가 재개된다.
	ModeChanged,       // 해상도가 바뀌어 프레임 리소스를 다시 만들었다.
	                   // 이전에 받아간 텍스처와 공유 핸들은 무효다.
	DeviceRemoved,     // D3D 디바이스 상실.
	DeviceRecreated,   // 디바이스 재생성 성공. 이전 텍스처는 모두 무효다.
	Faulted,           // 복구 불가. 캡처 스레드가 유휴 상태로 전환된다.
};

// 캡처 스레드에서 호출된다. 블로킹 작업을 하면 안 된다.
using CaptureEventCallback = void(*)(CaptureEventCode code, HRESULT hr, void* userData);

struct CaptureStats
{
	uint64_t capturedFrames = 0;        // 풀/공유 텍스처에 실제로 발행한 프레임
	uint64_t skippedFrames = 0;         // 화면 변화가 없어 발행을 건너뛴 프레임
	uint64_t droppedFrames = 0;         // 소비자가 밀려 버린 프레임
	uint64_t timeoutCount = 0;          // AcquireNextFrame 타임아웃 횟수
	uint64_t accessLostCount = 0;       // duplication 접근 상실 횟수
	uint64_t reconnectCount = 0;        // 재연결 성공 횟수
	uint64_t deviceRecreateCount = 0;   // 디바이스 재생성 횟수
	uint64_t invalidReleaseCount = 0;   // 잘못된 핸들 반납 횟수(호출자 버그 지표)
	HRESULT lastError = S_OK;
};

enum FrameStatus : LONG
{
	EMPTY = 0,
	READY,
	BUSY,
};


// 공유 프레임 풀의 keyed mutex 규약.
//
// 키를 하나만 쓴다. 생산자 키 / 소비자 키를 번갈아 쓰는 방식은 두 쪽이
// 반드시 짝을 맞춰 주고받을 때만 성립하는데, 이 풀은 latest-only 로
// 소비되어 "소비자가 손도 대지 않은 채 버려지는 프레임" 이 정상 경로다.
// 그때 키가 소비자 쪽에 걸린 채 남아 그 슬롯이 영구히 죽는다.
//
// 키 하나면 어느 쪽이든 아무 때나 잡을 수 있고, 논리적 소유권은 슬롯의
// status / referenceCount 가 이미 처리한다. keyed mutex 는 디바이스 간
// GPU 동기화만 맡으면 되고 그게 원래 필요한 전부다.
constexpr UINT64 FRAME_POOL_MUTEX_KEY = 0;

// 잡지 못하면 그 프레임을 버린다. INFINITE 는 쓰지 않는다 —
// 이 구조를 도입한 이유 자체가 무기한 대기를 없애는 것이었다.
constexpr DWORD FRAME_POOL_MUTEX_TIMEOUT_MS = 100;
struct CapturedFrameSlot
{
	ID3D11Texture2D* texture = nullptr;
	
	// 프레임 풀을 다른 D3D11 디바이스와 공유할 때만 채워진다.
	// (SetFramePoolSharable)
	//
	// sharedHandle 은 소비자가 OpenSharedResource1 로 열어 갈 핸들이고,
	// keyedMutex 는 이 텍스처를 건드리는 모든 디바이스가 공유하는 동기화
	// 객체다. 우리 쪽 CopyResource 도 반드시 이걸 잡고 해야 한다 —
	// 한쪽만 지키면 지키지 않는 것과 같다.
	HANDLE sharedHandle = nullptr;
	IDXGIKeyedMutex* keyedMutex = nullptr;
	ID3D11Query* copyDoneQuery = nullptr;
	DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
	PTR_INFO mouseInfo = {};

	volatile LONG referenceCount = 0;
	volatile LONG status = FrameStatus::EMPTY;
	volatile LONG64 frameId = 0;
};

struct CapturedFrameHandle
{
	ID3D11Texture2D* texture = nullptr;
	LONG slotId = -1;
	uint64_t frameId = 0ULL;
};
