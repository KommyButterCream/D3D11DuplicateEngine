#include "pch.h"
#include "D3D11DuplicateThread.h"
#include "D3D11DuplicateEngine.h"

#include "../../../Module/Core/Concurrency/WaitableTimer.h"

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
//
// 고해상도 타이머 생성과 폴백, 자동 리셋을 쓰는 이유는
// Core::Concurrency::WaitableTimer 주석에 정리되어 있다.
void D3D11DuplicateThread::Run()
{
	using WaitableTimer = Core::Concurrency::WaitableTimer;

	if (!m_duplicateEngine)
		return;

	WaitableTimer timer;
	uint64_t previousFps = 0;

	while (!IsStopRequested())
	{
		const uint64_t fps = m_duplicateEngine->GetTargetFps();

		if (fps == 0 || !timer.IsValid())
		{
			// fps 가 0 일때에는 대기 없는 무제한 캡쳐모드이다.
			// 그러므로 기존에 등록된 타이머가 있으면 취소시키고
			// 화면 프레임을 캡쳐한 후 바로 다음 프레임을 캡쳐하러 continue 한다.
			if (previousFps != 0 && timer.IsValid())
			{
				// 캡쳐 Target FPS 가 x 에서 0으로 변경된 경우
				// 기존 타이머를 취소한다.
				timer.Cancel();
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
			// fps 로부터 1-frame capture time ms 계산.
			// fps 가 1000 을 넘어 0 이 되면 SignalEvery 이 1ms 로 올려준다.
			const uint32_t period_ms = static_cast<uint32_t>(1000ULL / fps);

			// 첫 신호는 즉시(0ms) 받는다.
			// fps = 60 이면 period = 1000 / 60 = 16ms 이므로
			// 최초 신호 이후 16ms 마다 타이머가 다시 신호된다.
			// Sleep(16ms) 보다 훨씬 정밀하다.
			if (!timer.SignalEvery(0.0, period_ms))
			{
				// 타이머를 못 걸면 무제한 모드로 떨어진다. 멈추는 것보다 낫다.
				timer.Close();
				previousFps = 0;
				continue;
			}

			previousFps = fps;
		}

		// 타이머와 정지 이벤트를 함께 기다린다.
		const WaitableTimer::WaitResult waitResult = timer.Wait(GetStopEvent());

		if (waitResult == WaitableTimer::WaitResult::Stopped)
		{
			// 정지 요청
			break;
		}

		if (waitResult != WaitableTimer::WaitResult::Signaled)
		{
			// 대기 실패. 타이머를 포기하고 무제한 모드로 계속한다.
			timer.Close();
			previousFps = 0;
			continue;
		}

		// 화면 프레임 캡쳐 진행
		// 여기는 사용자가 설정한 주기 마다 화면 캡쳐를 수행해주는 곳.
		m_duplicateEngine->ProcessCaptureFrame();
	}

	// timer 소멸자가 CancelWaitableTimer + CloseHandle 을 처리한다.
}