#pragma once

namespace DebuggingUtil
{
	uint32_t FNV1A32(const void *Input, size_t Length);
	void SetObjectDebugName(ID3D12Object *Object, const char *Name);
}
