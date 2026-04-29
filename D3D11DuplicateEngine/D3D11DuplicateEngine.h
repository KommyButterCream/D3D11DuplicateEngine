#pragma once

#ifdef BUILD_D3D11_DUPLICATE_ENGINE
#define D3D11_DUPLICATE_ENGINE_API __declspec(dllexport)
#else
#define D3D11_DUPLICATE_ENGINE_API __declspec(dllimport)
#endif

#include <stdint.h>
#include "CommonTypes.h"

class D3D11RenderEngine;
class D3D11ImageIO;
class D3D11DuplicateThread;
struct IDXGIOutput1;
struct IDXGIOutputDuplication;

using FrameCallback = void(*)(void* userData);

class D3D11_DUPLICATE_ENGINE_API D3D11DuplicateEngine
{
public:
	D3D11DuplicateEngine() = default;
	~D3D11DuplicateEngine();

	bool Initialize(uint32_t outputIndex = 0);
	bool IsInitialized() const { return m_initialized; }
	void Shutdown();

	void SetTargetFps(uint64_t fps);
	uint64_t GetTargetFps() const;

	uint32_t GetOutputCount() const;
	uint32_t GetOutputWidth();
	uint32_t GetOutputHeight();

	bool AcquireFrame(UINT timeout_ms, CaptureFrameResult& outResult);
	void ReleaseFrame();

	// Capture Thread
	bool StartThread();
	void StopThread();

	void SetFrameCaptureCallback(FrameCallback funcCallback, void* userData); // 스레드에서 호출 할 함수 등록

	CapturedFrameHandle GetLatestFrameHandle();
	void ReleaseLatestFrameHandle(CapturedFrameHandle& handle);

	ID3D11Device1* GetD3DDevice();

private:
	friend class D3D11DuplicateThread;

	HRESULT InitializeDuplication(uint32_t outputIndex);
	HRESULT CreateSharedTexture(UINT width, UINT height, ID3D11Texture2D** texture, HANDLE* sharedHandle);

	bool InitializeCaptureFramePool();
	void DestroyCaptureFramePool();
	void CopyCaptureTextureToPool(ID3D11Texture2D* capturedTexture);
	LONG GetLatestFrameID();

	bool UpdateMouseInfo(DXGI_OUTDUPL_FRAME_INFO& frameInfo);
	bool UpdateDirtyMoveInfo(DXGI_OUTDUPL_FRAME_INFO& frameInfo, CaptureFrameResult& outResult);

	// Capture Thread
	void ProcessCaptureFrame();

private:
	bool m_initialized = false;
	uint32_t m_outputIndex = 0;
	bool m_useMouseInfo = false;
	bool m_useMoveDiryInfo = false;

	// Render Engine
	D3D11RenderEngine* m_D3D11Engine = nullptr;

	IDXGIOutput1* m_dxgiOutput = nullptr;
	IDXGIOutputDuplication* m_deskDupl = nullptr;

	DXGI_OUTDUPL_DESC m_duplDesc = {};
	DXGI_OUTPUT_DESC m_outputDesc = {};

	// Capture Frame per second
	uint64_t m_captureFPS = 0;

	// Capture Result
	static constexpr size_t POOL_COUNT = 4;
	CapturedFrameSlot m_framePool[POOL_COUNT];
	static_assert((POOL_COUNT& (POOL_COUNT - 1)) == 0, "Pool count must be power of two");
	alignas(64) LONG m_latestFrameId = 0;


	// Capture Image
	bool m_enableSharedTexture = true;
	HANDLE m_sharedHandle = nullptr;
	ID3D11Texture2D* m_capturedTexture = nullptr; // 현재 잡고 있는 프레임
	ID3D11Texture2D* m_sharedTexture = nullptr; // 로컬 공유용 버퍼
	bool m_frameAcquired = false;

	BYTE* m_metaDataBuffer = nullptr;
	UINT m_metaDataSize = 0;
	PTR_INFO m_mouseInfo = {};          // 최신 마우스 상태

	// Capture Thread
	D3D11DuplicateThread* m_duplicateThread = nullptr;

	FrameCallback m_frameCallback = nullptr;
	void* m_userData = nullptr;

	// Debug
	D3D11ImageIO* m_WICImageIO = nullptr;
};

