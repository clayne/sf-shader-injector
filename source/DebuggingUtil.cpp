#include <cstdlib>
#include "DebuggingUtil.h"
#include "Plugin.h"

namespace DebuggingUtil
{
	uint32_t FNV1A32(const void *Input, size_t Length)
	{
		constexpr uint32_t FNV1_PRIME_32 = 0x01000193;
		constexpr uint32_t FNV1_BASE_32 = 2166136261U;

		auto data = reinterpret_cast<const unsigned char *>(Input);
		auto end = data + Length;

		auto hash = FNV1_BASE_32;

		for (; data != end; data++)
		{
			hash ^= *data;
			hash *= FNV1_PRIME_32;
		}

		return hash;
	}

	void SetObjectDebugName(ID3D12Object *Object, const char *Name)
	{
		if (!Plugin::InsertDebugMarkers)
			return;

		if (!Object || !Name || strlen(Name) <= 0)
			return;

		wchar_t tempOut[1024];
		if (mbstowcs_s(nullptr, tempOut, Name, _TRUNCATE) == 0)
			Object->SetName(tempOut);
	}

	void (*OriginalCreateTexture)(void *, void *, void *, void *, const char *, void *, void *);
	void HookedCreateTexture(void *a1, void *a2, void *a3, void *a4, const char *DebugName, void *a6, void *a7)
	{
		OriginalCreateTexture(a1, a2, a3, a4, DebugName, a6, a7);

		auto textureResource = *reinterpret_cast<void **>(a4);
		auto dx12TextureResource = *reinterpret_cast<ID3D12Resource **>(reinterpret_cast<uintptr_t>(textureResource) + 0x78);

		SetObjectDebugName(dx12TextureResource, DebugName);
	}

	void (*OriginalCmdBeginProfilingMarker)(void *, void *, const char *);
	void HookedCmdBeginProfilingMarker(void *a1, void *a2, const char *MarkerText)
	{
		auto commandList = *reinterpret_cast<ID3D12GraphicsCommandList **>(reinterpret_cast<uintptr_t>(a1) + 0x10);
		commandList->BeginEvent(1, MarkerText, static_cast<uint32_t>(strlen(MarkerText) + 1));

		OriginalCmdBeginProfilingMarker(a1, a2, MarkerText);
	}

	void (*OriginalCmdEndProfilingMarker)(void *);
	void HookedCmdEndProfilingMarker(void *a1)
	{
		OriginalCmdEndProfilingMarker(a1);

		auto commandList = *reinterpret_cast<ID3D12GraphicsCommandList **>(reinterpret_cast<uintptr_t>(a1) + 0x10);
		commandList->EndEvent();
	}

	DECLARE_HOOK_TRANSACTION(DebuggingUtil)
	{
		if (!Plugin::InsertDebugMarkers)
			return;

		Hooks::WriteJump(
			Offsets::Signature(
				"4C 89 4C 24 20 4C 89 44 24 18 48 89 54 24 10 48 89 4C 24 08 53 56 57 41 54 41 55 41 56 41 57 48 81 EC D0 02 00 00"),
			&HookedCreateTexture,
			&OriginalCreateTexture);

		Hooks::WriteJump(
			Offsets::Signature("48 89 5C 24 08 48 89 74 24 10 44 88 4C 24 20 57 48 83 EC 20"),
			&HookedCmdBeginProfilingMarker,
			&OriginalCmdBeginProfilingMarker);

		Hooks::WriteJump(
			Offsets::Signature("48 89 5C 24 08 88 54 24 10 57 48 83 EC 20 48 8B F9 E8 ? ? ? ? 8B D8 89 44 24 38 B9 1A 00 00 00"),
			&HookedCmdEndProfilingMarker,
			&OriginalCmdEndProfilingMarker);
	};
}
