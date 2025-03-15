#include "CComPtr.h"
#include "D3DPipelineStateStream.h"
#include "D3DShaderReplacement.h"
#include "DebuggingUtil.h"
#include "Plugin.h"

namespace D3DShaderReplacement
{
	const std::filesystem::path& GetShaderBinDirectory()
	{
		const static auto path = []()
		{
			auto temp = Plugin::ShaderDumpBinPath;

			if (temp.empty())
				temp = std::filesystem::current_path() / "Data" / "shadersfx";

			spdlog::info("Using custom shader root directory: {}", temp.string());
			return temp;
		}();

		return path;
	}

	const char *GetShaderTypePrefix(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type)
	{
		switch (Type)
		{
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
			return "vs";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
			return "ps";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
			return "hs";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
			return "ds";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
			return "gs";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
			return "cs";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
			return "as";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
			return "ms";
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
			return "rsg";
		}

		return "unknown";
	}

	bool ExtractOrReplaceShader(
		D3DPipelineStateStream::Copy& StreamCopy,
		D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type,
		D3D12_SHADER_BYTECODE *Bytecode,
		const char *TechniqueName,
		uint64_t TechniqueId)
	{
		// Techniques have to be trimmed as they're too long to be used in file names
		const auto prefix = GetShaderTypePrefix(Type);

		char techniqueShortName[512] = {};
		strncpy_s(techniqueShortName, TechniqueName, _TRUNCATE);

		if (auto s = strchr(techniqueShortName, '-'))
			*s = '\0';

		char shaderBinFileName[512];
		sprintf_s(shaderBinFileName, "%s_%llX_%s.bin", techniqueShortName, TechniqueId, prefix);

		const auto shaderBinFullPath = GetShaderBinDirectory() / techniqueShortName / shaderBinFileName;

		if (!Plugin::ShaderDumpBinPath.empty())
		{
			// Extract it
			if (Bytecode->pShaderBytecode && Bytecode->BytecodeLength != 0)
			{
				// Calculate the shader data hash, dump it, then map it to a technique name in a dedicated CSV file. It's
				// far from efficient but it's usually a one-time operation.
				std::filesystem::create_directories(shaderBinFullPath.parent_path());

				const auto hash = DebuggingUtil::FNV1A32(Bytecode->pShaderBytecode, Bytecode->BytecodeLength);
				spdlog::info("Dumping shader with hash {} to {}", hash, shaderBinFullPath.string());

				static std::mutex fileDumpMutex;
				fileDumpMutex.lock();
				{
					// Dump binary
					if (std::ofstream f(shaderBinFullPath, std::ios::binary); f.good())
						f.write(reinterpret_cast<const char *>(Bytecode->pShaderBytecode), Bytecode->BytecodeLength);

					// Append to CSV
					const static auto csvPath = Plugin::ShaderDumpBinPath / "ShaderTechniqueMap.csv";

					if (std::ofstream f(csvPath, std::ios::app); f.good())
					{
						char csvLine[2048];
						auto length = sprintf_s(
							csvLine,
							"%s,%s,%u,%llX,\"%s\"\n",
							techniqueShortName,
							prefix,
							hash,
							TechniqueId,
							TechniqueName);

						f.write(csvLine, length);
					}
				}
				fileDumpMutex.unlock();
			}
		}
		else
		{
			// Replace it
			if (std::ifstream f(shaderBinFullPath, std::ios::binary | std::ios::ate); f.good())
			{
				static bool once = [&]()
				{
					spdlog::info("Trying to replace at least one shader: {}", shaderBinFullPath.string());
					return true;
				}();

				auto fileSize = static_cast<uint64_t>(f.tellg());
				auto fileData = std::make_unique<uint8_t[]>(fileSize);

				f.seekg(0, std::ios::beg);
				f.read(reinterpret_cast<char *>(fileData.get()), fileSize);

				// Only replace if the on-disk data is different
				if (fileSize != Bytecode->BytecodeLength || memcmp(fileData.get(), Bytecode->pShaderBytecode, fileSize) != 0)
				{
					Bytecode->BytecodeLength = fileSize;
					Bytecode->pShaderBytecode = fileData.get();
					StreamCopy.TrackAllocation(std::move(fileData));

					spdlog::trace("Used file replacement: {}", shaderBinFullPath.string());
					return true;
				}
			}
		}

		return false;
	}

	bool PatchPipelineStateStream(
		D3DPipelineStateStream::Copy& StreamCopy,
		ID3D12Device2 *Device,
		const std::span<const uint8_t> *RootSignatureData,
		const char *TechniqueName,
		uint64_t TechniqueId)
	{
		bool modified = false;

		for (D3DPipelineStateStream::Iterator iter(StreamCopy.GetDesc()); !iter.AtEnd(); iter.Advance())
		{
			switch (auto obj = iter.GetObj(); obj->Type)
			{
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
				if (ExtractOrReplaceShader(StreamCopy, obj->Type, &obj->Shader, TechniqueName, TechniqueId))
					modified = true;
				break;

			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
				if (RootSignatureData)
				{
					D3D12_SHADER_BYTECODE bytecode {
						.pShaderBytecode = RootSignatureData->data(),
						.BytecodeLength = RootSignatureData->size(),
					};

					if (ExtractOrReplaceShader(StreamCopy, obj->Type, &bytecode, TechniqueName, TechniqueId))
					{
						CComPtr<ID3D12RootSignature> newSignature;
						const auto hr = Device->CreateRootSignature(
							0,
							bytecode.pShaderBytecode,
							bytecode.BytecodeLength,
							IID_PPV_ARGS(&newSignature));

						if (FAILED(hr))
						{
							// Somebody passed in malformed data
							spdlog::error(
								"Failed to create root signature: {:X}. Shader technique: {:X}.",
								static_cast<uint32_t>(hr),
								TechniqueId);
						}
						else
						{
							obj->RootSignature = newSignature.Get();
							StreamCopy.TrackObject(std::move(newSignature));

							modified = true;
						}
					}
				}
				break;
			}
		}

		// Loop around once again to disable PSO cache entries
		if (modified)
		{
			for (D3DPipelineStateStream::Iterator iter(StreamCopy.GetDesc()); !iter.AtEnd(); iter.Advance())
			{
				switch (auto obj = iter.GetObj(); obj->Type)
				{
				case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
					obj->CachedPSO.pCachedBlob = nullptr;
					obj->CachedPSO.CachedBlobSizeInBytes = 0;
					break;
				}
			}
		}

		return modified;
	}
}
