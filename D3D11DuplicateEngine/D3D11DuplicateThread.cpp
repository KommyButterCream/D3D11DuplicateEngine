#include "pch.h"
#include "D3D11DuplicateThread.h"
#include "D3D11DuplicateEngine.h"

D3D11DuplicateThread::D3D11DuplicateThread(D3D11DuplicateEngine* duplicateEngine)
	: Core::Concurrency::ThreadBase(L"D3D11CaptureThread")
	, m_duplicateEngine(duplicateEngine)
{
}

void D3D11DuplicateThread::Run()
{
	LARGE_INTEGER frequency = {};
	LARGE_INTEGER next = {};

	::QueryPerformanceFrequency(&frequency);
	::QueryPerformanceCounter(&next);

	while (!IsStopRequested())
	{
		if (!m_duplicateEngine)
		{
			break;
		}

		const uint64_t fps = m_duplicateEngine->GetTargetFps();

		if (fps == 0)
		{
			m_duplicateEngine->ProcessCaptureFrame();
			continue;
		}

		LONGLONG interval = frequency.QuadPart / fps;
		if (interval <= 0)
		{
			interval = 1;
		}

		LARGE_INTEGER now = {};
		::QueryPerformanceCounter(&now);

		if (now.QuadPart < next.QuadPart)
		{
			LONGLONG remain = next.QuadPart - now.QuadPart;
			DWORD sleep_ms = static_cast<DWORD>((remain * 1000) / frequency.QuadPart);

			if (sleep_ms > 1)
			{
				::Sleep(sleep_ms - 1);
			}

			continue;
		}

		m_duplicateEngine->ProcessCaptureFrame();

		next.QuadPart += interval;

		::QueryPerformanceCounter(&now);

		if (now.QuadPart >= next.QuadPart)
		{
			next.QuadPart = now.QuadPart + interval;
		}
	}
}
