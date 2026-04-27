#pragma once

#include "../../../Module/Core/Concurrency/ThreadBase.h"

class D3D11DuplicateEngine;

class D3D11DuplicateThread final : public Core::Concurrency::ThreadBase
{
public:
	explicit D3D11DuplicateThread(D3D11DuplicateEngine* duplicateEngine);

protected:
	void Run() override;

private:
	D3D11DuplicateEngine* m_duplicateEngine = nullptr;
};
