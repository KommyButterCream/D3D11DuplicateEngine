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
	while (!IsStopRequested())
	{
		if (!m_duplicateEngine)
		{
			break;
		}

		m_duplicateEngine->ProcessCaptureFrame();
	}
}
