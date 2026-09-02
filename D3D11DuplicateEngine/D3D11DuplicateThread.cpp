#include "pch.h"
#include "D3D11DuplicateThread.h"
#include "D3D11DuplicateEngine.h"

namespace
{
	// Win10 1803 부터 1ms 단위 정밀도를 준다. 없으면 일반 타이머로 떨어지는데,
	// 그 경우 기본 타이머 해상도(약 15.6ms)에 묶인다.
	// 플래그 0 = 자동 리셋(동기화 타이머). 대기 한 번이 신호 하나를 소비하므로
	// 주기 타이머의 페이싱이 그대로 유지된다. 수동 리셋으로 만들면 한 번 신호된
	// 뒤 계속 신호 상태로 남아 대기가 즉시 통과해 버린다.
	HANDLE CreateFrameTimer()
	{
#if defined(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)
		HANDLE timer = ::CreateWaitableTimerExW(
			nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
		if (timer)
			return timer;
#endif
		// 고해상도 플래그를 지원하지 않는 환경(Win10 1803 미만).
		return ::CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
	}
}

D3D11DuplicateThread::D3D11DuplicateThread(D3D11DuplicateEngine* duplicateEngine)
	: Core::Concurrency::ThreadBase(L"D3D11CaptureThread")
	, m_duplicateEngine(duplicateEngine)
{
}

// 프레임 페이싱은 주기 waitable timer 가 담당한다.
//
// 예전에는 QPC 로 다음 시각을 계산하고 Sleep 으로 맞췄는데, Sleep 은 기본
// 타이머 해상도(약 15.6ms)에 묶여 있어서 30fps(33.3ms) 목표가 31.2ms 또는
// 46.8ms 로 튀었다. 남는 1ms 미만 구간은 양보 없이 스핀해 코어도 태웠다.
//
// 주기 타이머는 처리 시간이 주기를 넘겨도 밀린 주기를 합쳐서 즉시 신호하므로,
// 드리프트가 누적되지 않고 늦은 프레임은 곧바로 따라간다.
// 정지 요청은 stop 이벤트를 같이 기다려 즉시 반응한다.
void D3D11DuplicateThread::Run()
{
	if (!m_duplicateEngine)
		return;

	HANDLE timer = CreateFrameTimer();
	uint64_t previousFps = 0;

	while (!IsStopRequested())
	{
		const uint64_t fps = m_duplicateEngine->GetTargetFps();

		if (fps == 0 || !timer)
		{
			// fps 가 0 일때에는 대기 없는 무제한 캡쳐모드이다.
			// 그러므로 기존에 등록된 타이머가 있으면 취소시키고
			// 화면 프레임을 캡쳐한 후 바로 다음 프레임을 캡쳐하러 continue 한다.
			if (previousFps != 0 && timer)
			{
				// 캡쳐 Target FPS 가 x 에서 0으로 변경된 경우
				// 기존 타이머를 취소한다.
				::CancelWaitableTimer(timer);
				previousFps = 0;
			}

			// 화면 프레임 캡쳐 진행
			m_duplicateEngine->ProcessCaptureFrame();

			continue;
		}

		// 무제한 캡쳐모드가 아닌 경우
		// 일반적으로 캡쳐 FPS 가 설정되어 있는 경우이다.
		if (fps != previousFps)
		{
			// 타이머 설정은 fps 가 변경된 경우에만 수행한다.

			// period_ms : 반복 주기 (ms)
			// fps 로부터 1-frame capture time ms 계산
			LONG period_ms = static_cast<LONG>(1000ULL / fps);

			// 최소 주기(ms) 를 1ms 로 강제. 0 이 되지 않도록 한다.
			if (period_ms <= 0)
				period_ms = 1;

			// 음수 = 상대 시각(100ns 단위)
			// 현재 시간으로부터 100ns 이후니 즉시 첫 신호가 발생된다.
			LARGE_INTEGER dueTime_100ns = {};
			dueTime_100ns.QuadPart = -1LL; // 100ns

			// fps = 60 이면 period = 1000 / 60 = 16ms 이므로
			// 최초 신호 이후 16ms 마다 타이머가 다시 신호된다.
			// Sleep(16ms) 보다 훨씬 정밀하다.
			if (!::SetWaitableTimer(timer, &dueTime_100ns, period_ms, nullptr, nullptr, FALSE))
			{
				// 타이머를 못 걸면 무제한 모드로 떨어진다. 멈추는 것보다 낫다.
				::CloseHandle(timer);
				timer = nullptr;
				previousFps = 0;
				continue;
			}

			previousFps = fps;
		}

		// timer 는 루프 안에서 교체될 수 있으므로 매번 새로 구성한다.
		const HANDLE waitHandles[2] = { GetStopEvent(), timer };
		const DWORD waitResult = ::WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
		if (waitResult == WAIT_OBJECT_0)
		{
			// 정지 요청
			break;
		}

		if (waitResult != WAIT_OBJECT_0 + 1)
		{
			// 대기 실패. 타이머를 포기하고 무제한 모드로 계속한다.
			::CloseHandle(timer);
			timer = nullptr;
			previousFps = 0;
			continue;
		}

		// waitResult 가 WAIT_OBJECT_0 + 1 인 경우
		// 화면 프레임 캡쳐 진행
		// 여기는 사용자가 설정한 주기 마다 화면 캡쳐를 수행해주는 곳.
		m_duplicateEngine->ProcessCaptureFrame();
	}

	if (timer)
	{
		::CancelWaitableTimer(timer);
		::CloseHandle(timer);
	}
}
