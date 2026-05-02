#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d11_1.h>
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
	HANDLE sharedHandle = nullptr; // 로컬 뷰어용 공유 핸들
	DXGI_OUTDUPL_FRAME_INFO frameInfo = {};   // 프레임 메타데이터 (Dirty Rects 등)
	BYTE* metaData = nullptr;
	UINT dirtyCount = 0;
	UINT moveCount = 0;
	PTR_INFO mouseInfo = {};   // 마우스 위치 및 모양
};

enum FrameStatus : LONG
{
	EMPTY = 0,
	READY,
	BUSY,
};

struct CapturedFrameSlot
{
	ID3D11Texture2D* texture = nullptr;
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

struct SharedCaptureData
{
	HANDLE sharedHandle = nullptr;
	SRWLOCK lock = SRWLOCK_INIT;
	bool newFrame = false;
};

class D3D11ImageView;

struct CaptureCallbackContext
{
	bool isProcessing = false;
	void* ownerData = nullptr;

	CaptureFrameResult* captureFrame = nullptr;

	SharedCaptureData* sharedData = nullptr;
	D3D11ImageView* imageView = nullptr;
	ID3D11Device* D3D11Device = nullptr;

	HANDLE lastHandle = nullptr;
	ID3D11Texture2D* opendTexture = nullptr;

	ID3D11Texture2D* stagingTex = nullptr; // CPU 읽기용 텍스처
	uint32_t lastWidth = 0;
	uint32_t lastHeight = 0;
};
