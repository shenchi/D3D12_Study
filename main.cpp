#include <Windows.h>
#include <cstdio>
#include <dxgi1_5.h>
#include <d3d12.h>
#include <DirectXMath.h>

#include <WICTextureLoader.h>
//#include <d3dx12.h>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d12.lib")

#define CHECKED(x) if (!SUCCEEDED(x)) { return false; }

#include "Mesh.h"

namespace
{
	struct Memory
	{
		void*	ptr;
		size_t	size;

		Memory() : ptr(nullptr), size(0) {}
		Memory(size_t size) : ptr(nullptr), size(size) { ptr = reinterpret_cast<void*>(new char[size]); }

		~Memory() { if (nullptr != ptr) delete[] reinterpret_cast<char*>(ptr); }

		Memory(const Memory&) = delete;
		Memory(Memory&& m) : ptr(m.ptr), size(m.size) { m.ptr = nullptr; m.size = 0; }

		void LoadFromFile(const char* filename)
		{
			FILE* fp = fopen(filename, "rb");
			if (nullptr == fp) return;

			if (nullptr != ptr)
			{
				delete[] reinterpret_cast<char*>(ptr);
				size = 0;
			}

			fseek(fp, 0, SEEK_END);
			size_t length = ftell(fp);
			fseek(fp, 0, SEEK_SET);

			size = length;
			ptr = reinterpret_cast<void*>(new char[size]);
			size_t length_read = fread(ptr, 1, length, fp);
			fclose(fp);

			if (length != length_read)
			{
				delete[] reinterpret_cast<char*>(ptr);
				size = 0;
			}
		}
	};

	class Application
	{
	public:

		bool Init(HWND hWnd, int width, int height)
		{
			this->hWnd = hWnd;
			this->width = width;
			this->height = height;

			if (!InitDirect3D()) return false;
			if (!InitAssets()) return false;

			return true;
		}

		void Release()
		{
			WaitForGPU();
			ReleaseAssets();
			ReleaseDirect3D();
		}

		void Update()
		{
			QueryPerformanceCounter(&currentCounter);
			lastCounter.QuadPart = currentCounter.QuadPart - lastCounter.QuadPart;
			lastCounter.QuadPart *= 1000000;
			lastCounter.QuadPart /= counterFreq.QuadPart;

			timeDelta = lastCounter.QuadPart / 1000000.0f;
			timeElapsed += timeDelta;

			lastCounter = currentCounter;

			{
				wchar_t title[256] = {};
				int fps = static_cast<int>(1.0f / timeDelta);
				wsprintf(title, L"D3D12_Study       FPS: %i", fps);
				SetWindowText(hWnd, title);
			}

			Update(timeDelta);
			Render();
		}

		bool OnResize(int width, int height)
		{
			if (nullptr == swapChain)
				return false;

			//CHECKED(swapChain->ResizeBuffers(2, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0));

			return true;
		}

	private:

		bool InitDirect3D()
		{
			UINT dxgi_flag = 0;
			IDXGIFactory5* factory = nullptr;
			IDXGIAdapter1* adaptor = nullptr;

#if defined(_DEBUG)
			{
				ID3D12Debug* debug = nullptr;
				if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
				{
					debug->EnableDebugLayer();
					debug->Release();

					dxgi_flag |= DXGI_CREATE_FACTORY_DEBUG;
				}
			}
#endif

			CHECKED(CreateDXGIFactory2(dxgi_flag, IID_PPV_ARGS(&factory)));

			{


				IDXGIAdapter1* adpt = nullptr;
				for (UINT i = 0;
					SUCCEEDED(factory->EnumAdapters1(i, &adpt));
					i++)
				{
					DXGI_ADAPTER_DESC1 desc;
					CHECKED(adpt->GetDesc1(&desc));

					if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0)
					{
						adaptor = adpt;
						break;
					}

					adpt->Release();
				}

				if (nullptr == adaptor)
					return false;

			}

			CHECKED(D3D12CreateDevice(adaptor, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)));
			adaptor->Release();

			{
				D3D12_COMMAND_QUEUE_DESC desc = {};
				desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
				CHECKED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&cmdQueue)));
			}

			{
				IDXGISwapChain1* swapChain_ = nullptr;
				DXGI_SWAP_CHAIN_DESC1 desc = {};
				desc.BufferCount = 2;
				desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				desc.Width = width;
				desc.Height = height;
				desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
				desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
				desc.SampleDesc.Count = 1;

				CHECKED(factory->CreateSwapChainForHwnd(cmdQueue, hWnd, &desc, nullptr, nullptr, &swapChain_));
				CHECKED(swapChain_->QueryInterface(IID_PPV_ARGS(&swapChain)));
				swapChain_->Release();

				backBufferIndex = swapChain->GetCurrentBackBufferIndex();
			}

			factory->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER);
			factory->Release();

			{
				D3D12_DESCRIPTOR_HEAP_DESC desc = {};
				desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
				desc.NumDescriptors = 2;
				CHECKED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&rtvHeap)));
				rtvHeapInc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
			}

			{
				auto handle = rtvHeap->GetCPUDescriptorHandleForHeapStart();

				for (UINT i = 0; i < 2; ++i)
				{
					CHECKED(swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffers[i])));
					device->CreateRenderTargetView(backBuffers[i], nullptr, handle);
					handle.ptr += rtvHeapInc;
				}
			}

			{
				D3D12_DESCRIPTOR_HEAP_DESC desc = {};
				desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
				desc.NumDescriptors = 1;
				CHECKED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&dsvHeap)));
			}

			{
				D3D12_HEAP_PROPERTIES prop = {};
				prop.Type = D3D12_HEAP_TYPE_DEFAULT;

				D3D12_RESOURCE_DESC desc = {};
				desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
				desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
				desc.Width = width;
				desc.Height = height;
				desc.DepthOrArraySize = 1;
				desc.MipLevels = 1;
				desc.SampleDesc.Count = 1;
				desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

				D3D12_CLEAR_VALUE clearValue[1];
				clearValue[0].Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
				clearValue[0].DepthStencil.Depth = 1.0f;
				clearValue[0].DepthStencil.Stencil = 0;

				CHECKED(device->CreateCommittedResource(&prop, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, clearValue, IID_PPV_ARGS(&depthBuffer)));

				device->CreateDepthStencilView(depthBuffer, nullptr, dsvHeap->GetCPUDescriptorHandleForHeapStart());
			}

			CHECKED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc)));

			CHECKED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc, nullptr, IID_PPV_ARGS(&cmdList)));

			cmdList->Close();

			frameIndex = 1;
			CHECKED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
			fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

			QueryPerformanceFrequency(&counterFreq);
			QueryPerformanceCounter(&lastCounter);
			timeElapsed = 0.0f;

			return true;
		}

		void ReleaseDirect3D()
		{
			CloseHandle(fenceEvent);

			fence->Release();
			cmdList->Release();
			cmdAlloc->Release();
			rtvHeap->Release();
			backBuffers[0]->Release();
			backBuffers[1]->Release();
			dsvHeap->Release();
			depthBuffer->Release();
			swapChain->Release();
			cmdQueue->Release();
			device->Release();
		}

		void WaitForGPU()
		{
			UINT64 v = frameIndex;
			cmdQueue->Signal(fence, frameIndex);
			frameIndex++;

			if (fence->GetCompletedValue() < v)
			{
				fence->SetEventOnCompletion(v, fenceEvent);
				WaitForSingleObject(fenceEvent, INFINITE);
			}

			backBufferIndex = swapChain->GetCurrentBackBufferIndex();
		}

	private:
		HWND						hWnd;
		int							width;
		int							height;
		LARGE_INTEGER				counterFreq;
		LARGE_INTEGER				lastCounter;
		LARGE_INTEGER				currentCounter;

		float						timeElapsed;
		float						timeDelta;

		ID3D12Device*				device;
		ID3D12CommandQueue*			cmdQueue;

		IDXGISwapChain4*			swapChain;

		ID3D12Resource*				backBuffers[2];

		ID3D12DescriptorHeap*		rtvHeap;
		UINT						rtvHeapInc;

		ID3D12Resource*				depthBuffer;
		ID3D12DescriptorHeap*		dsvHeap;

		ID3D12CommandAllocator*		cmdAlloc;
		ID3D12GraphicsCommandList*	cmdList;

		ID3D12Fence*				fence;
		HANDLE						fenceEvent;

		UINT						backBufferIndex;
		UINT64						frameIndex;

	private:

		struct ConstantsPerCamera
		{
			DirectX::XMFLOAT4X4		matView;
			DirectX::XMFLOAT4X4		matProj;
		};

		struct ConstantsPerInstance
		{
			DirectX::XMFLOAT4X4		matWorld;
			DirectX::XMFLOAT4X4		matWorldIT;
		};

		bool InitAssets()
		{
			{
				mesh.LoadFromFile("Assets/cube.fbx");

				D3D12_HEAP_PROPERTIES prop = {};
				prop.Type = D3D12_HEAP_TYPE_UPLOAD;

				D3D12_RESOURCE_DESC desc = {};
				desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				desc.DepthOrArraySize = 1;
				desc.Height = 1;
				desc.MipLevels = 1;
				desc.SampleDesc.Count = 1;
				desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
				desc.Width = mesh.GetVerticesCount() * Mesh::VertexSize;

				CHECKED(device->CreateCommittedResource(&prop, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vbRes)));

				D3D12_RANGE range = {};
				void* pData = nullptr;

				CHECKED(vbRes->Map(0, &range, &pData));
				mesh.FillInVerticesData(pData);
				vbRes->Unmap(0, nullptr);

				vbView = { vbRes->GetGPUVirtualAddress(), static_cast<UINT>(desc.Width), Mesh::VertexSize };

				desc.Width = sizeof(unsigned int) * mesh.GetIndicesCount();
				CHECKED(device->CreateCommittedResource(&prop, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ibRes)));

				CHECKED(ibRes->Map(0, &range, &pData));
				memcpy(pData, mesh.GetIndices(), desc.Width);
				ibRes->Unmap(0, nullptr);

				ibView = { ibRes->GetGPUVirtualAddress(), static_cast<UINT>(desc.Width), DXGI_FORMAT_R32_UINT };
			}

			{
				D3D12_HEAP_PROPERTIES prop = {};
				prop.Type = D3D12_HEAP_TYPE_UPLOAD;

				D3D12_RESOURCE_DESC desc = {};
				desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				desc.Width = sizeof(ConstantsPerCamera);
				desc.Height = 1;
				desc.DepthOrArraySize = 1;
				desc.MipLevels = 1;
				desc.SampleDesc.Count = 1;
				desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

				CHECKED(device->CreateCommittedResource(&prop, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&cbRes1)));
				CHECKED(device->CreateCommittedResource(&prop, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&cbRes2)));
			}

			{
				ConstantsPerCamera data;
				DirectX::XMStoreFloat4x4(
					&(data.matView),
					DirectX::XMMatrixTranspose(DirectX::XMMatrixTranslation(0.0f, 0.0f, 2.0f))
				);
				DirectX::XMStoreFloat4x4(
					&(data.matProj),
					DirectX::XMMatrixTranspose(
						DirectX::XMMatrixPerspectiveFovLH(DirectX::XM_PIDIV2, (float)width / height, 0.5f, 3.0f)
					)
				);

				D3D12_RANGE range = { 0, 0 };
				void* pData = nullptr;
				CHECKED(cbRes1->Map(0, &range, &pData));
				memcpy(pData, &data, sizeof(ConstantsPerCamera));
				cbRes1->Unmap(0, nullptr);
			}
			{
				ConstantsPerInstance data;
				DirectX::XMStoreFloat4x4(&(data.matWorld), DirectX::XMMatrixIdentity());
				DirectX::XMStoreFloat4x4(&(data.matWorldIT), DirectX::XMMatrixIdentity());

				D3D12_RANGE range = { 0, 0 };
				void* pData = nullptr;
				CHECKED(cbRes2->Map(0, &range, &pData));
				memcpy(pData, &data, sizeof(ConstantsPerInstance));
				cbRes2->Unmap(0, nullptr);
			}

			{
				ID3DBlob* blob = nullptr;

				D3D12_ROOT_SIGNATURE_DESC desc = {};
				desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

				D3D12_ROOT_PARAMETER param[4];
				param[0] = {};
				param[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
				param[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
				param[0].Constants.Num32BitValues = 4;

				D3D12_DESCRIPTOR_RANGE range[1];
				range[0] = {};
				range[0].NumDescriptors = 1;
				range[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;

				param[1] = {};
				param[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
				param[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
				param[1].DescriptorTable = { 1, range };

				param[2] = {};
				param[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
				param[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
				param[2].Descriptor = { 0, 0 };

				param[3] = {};
				param[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
				param[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
				param[3].Descriptor = { 1, 0 };


				desc.NumParameters = 4;
				desc.pParameters = param;

				D3D12_STATIC_SAMPLER_DESC sampler_desc[1];
				sampler_desc[0] = {};
				sampler_desc[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
				sampler_desc[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
				sampler_desc[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
				sampler_desc[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
				sampler_desc[0].MaxLOD = D3D12_FLOAT32_MAX;
				sampler_desc[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

				desc.NumStaticSamplers = 1;
				desc.pStaticSamplers = sampler_desc;

				CHECKED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, nullptr));
				CHECKED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSig)));
				blob->Release();
			}

			vs_mem.LoadFromFile("Assets/VertexShader.cso");
			ps_mem.LoadFromFile("Assets/PixelShader.cso");

			{
				D3D12_INPUT_ELEMENT_DESC inputElements[] =
				{
				  { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, Mesh::PositionOffset, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				  { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, Mesh::NormalOffset, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				  { "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, Mesh::TangentOffset, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				  { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, Mesh::UVOffset, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				};

				D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};

				desc.VS = { vs_mem.ptr, vs_mem.size };
				desc.PS = { ps_mem.ptr, ps_mem.size };
				desc.pRootSignature = rootSig;
				desc.InputLayout = { inputElements, 4 };
				desc.NumRenderTargets = 1;
				desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
				desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
				desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
				desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
				desc.NumRenderTargets = 1;
				desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
				desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
				desc.SampleDesc.Count = 1;
				desc.SampleMask = UINT_MAX;

				desc.DepthStencilState.DepthEnable = TRUE;
				desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
				desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;

				CHECKED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
			}

			{
				D3D12_DESCRIPTOR_HEAP_DESC desc = {};
				desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
				desc.NumDescriptors = 1;
				desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
				CHECKED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&srvHeap)));

				D3D12_SUBRESOURCE_DATA data = {};
				std::unique_ptr<uint8_t[]> ptr;

				CHECKED(CoInitializeEx(nullptr, COINITBASE_MULTITHREADED));
				CHECKED(DirectX::LoadWICTextureFromFile(device, L"Assets/wood.jpg", &tex, ptr, data));

				D3D12_RESOURCE_DESC resDesc = tex->GetDesc();
				UINT64 requiredSize = 0;
				device->GetCopyableFootprints(&resDesc, 0, 1, 0, nullptr, nullptr, nullptr, &requiredSize);
				//

				D3D12_HEAP_PROPERTIES prop = {};
				prop.Type = D3D12_HEAP_TYPE_UPLOAD;

				D3D12_RESOURCE_DESC uploadDesc = {};
				uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				uploadDesc.Width = requiredSize;
				uploadDesc.Height = 1;
				uploadDesc.DepthOrArraySize = 1;
				uploadDesc.MipLevels = 1;
				uploadDesc.SampleDesc.Count = 1;
				uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

				CHECKED(device->CreateCommittedResource(&prop, D3D12_HEAP_FLAG_NONE, &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadHeap)));

				cmdAlloc->Reset();
				cmdList->Reset(cmdAlloc, nullptr);

				void* pDestData = nullptr;
				D3D12_RANGE range = { 0, 0 };
				uploadHeap->Map(0, &range, &pDestData);
				memcpy(pDestData, reinterpret_cast<void*>(ptr.get()), data.SlicePitch);
				uploadHeap->Unmap(0, nullptr);

				D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
				D3D12_TEXTURE_COPY_LOCATION destLoc = {};

				srcLoc.pResource = uploadHeap;
				srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
				srcLoc.PlacedFootprint = { 0, {
					resDesc.Format, 
					static_cast<UINT>(resDesc.Width), 
					resDesc.Height, 
					static_cast<UINT>(resDesc.DepthOrArraySize), 
					static_cast<UINT>(data.RowPitch)
				} };

				destLoc.pResource = tex;
				destLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				destLoc.SubresourceIndex = 0;

				cmdList->CopyTextureRegion(&destLoc, 0, 0, 0, &srcLoc, nullptr);

				D3D12_RESOURCE_BARRIER barrier = {};
				barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
				barrier.Transition.pResource = tex;
				barrier.Transition.Subresource = 0;
				barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
				barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
				cmdList->ResourceBarrier(1, &barrier);

				cmdList->Close();

				ID3D12CommandList* lists[] = { cmdList };
				cmdQueue->ExecuteCommandLists(1, lists);
			}

			{
				D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
				srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				srvDesc.Texture2D.MipLevels = 1;
				srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				device->CreateShaderResourceView(tex, &srvDesc, srvHeap->GetCPUDescriptorHandleForHeapStart());
			}

			viewports[0] = { 0, 0, static_cast<FLOAT>(width), static_cast<FLOAT>(height), 0.0f, 1.0f };
			scissorRects[0] = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };

			WaitForGPU();
			return true;
		}

		void Update(float deltaTime)
		{
			D3D12_RANGE range = { 0, 0 };
			void* pData = nullptr;
			if (SUCCEEDED(cbRes2->Map(0, &range, &pData)))
			{
				DirectX::XMMATRIX world = DirectX::XMMatrixRotationAxis(DirectX::XMVectorSet(0, 1, 0, 0), timeElapsed);
				DirectX::XMStoreFloat4x4(
					reinterpret_cast<DirectX::XMFLOAT4X4*>(pData),
					DirectX::XMMatrixTranspose(world)
				);
				DirectX::XMStoreFloat4x4(
					reinterpret_cast<DirectX::XMFLOAT4X4*>(pData) + 1,
					DirectX::XMMatrixInverse(nullptr, world)
				);
				cbRes2->Unmap(0, nullptr);
			}
		}

		void Render()
		{
			cmdAlloc->Reset();
			cmdList->Reset(cmdAlloc, pso);

			cmdList->SetGraphicsRootSignature(rootSig);

			auto handle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
			handle.ptr += backBufferIndex * rtvHeapInc;

			float clearColor[] = { 0.7f, 0.7f, 0.7f, 1.0f };
			float blueColor[] = { 1.0f, 1.0f, 1.0f, 1.0f };

			D3D12_RESOURCE_BARRIER barriers[1];
			barriers[0] = {};
			barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barriers[0].Transition.pResource = backBuffers[backBufferIndex];
			barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
			barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
			cmdList->ResourceBarrier(1, barriers);

			cmdList->ClearRenderTargetView(handle, clearColor, 0, nullptr);
			cmdList->ClearDepthStencilView(dsvHeap->GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_DEPTH, 1.0, 0, 0, nullptr);

			cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			cmdList->IASetVertexBuffers(0, 1, &vbView);
			cmdList->IASetIndexBuffer(&ibView);

			cmdList->RSSetViewports(1, viewports);
			cmdList->RSSetScissorRects(1, scissorRects);

			cmdList->SetGraphicsRoot32BitConstants(0, 4, blueColor, 0);
			cmdList->SetDescriptorHeaps(1, &srvHeap);
			cmdList->SetGraphicsRootDescriptorTable(1, srvHeap->GetGPUDescriptorHandleForHeapStart());
			cmdList->SetGraphicsRootConstantBufferView(2, cbRes1->GetGPUVirtualAddress());
			cmdList->SetGraphicsRootConstantBufferView(3, cbRes2->GetGPUVirtualAddress());

			D3D12_CPU_DESCRIPTOR_HANDLE dsvHandles[1] = { dsvHeap->GetCPUDescriptorHandleForHeapStart() };
			cmdList->OMSetRenderTargets(1, &handle, FALSE, dsvHandles);
			cmdList->DrawInstanced(mesh.GetIndicesCount(), 1, 0, 0);

			barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
			cmdList->ResourceBarrier(1, barriers);

			cmdList->Close();

			ID3D12CommandList* cmdLists[] = { cmdList };
			cmdQueue->ExecuteCommandLists(1, cmdLists);

			swapChain->Present(0, 0);

			WaitForGPU();
		}

		void ReleaseAssets()
		{
			pso->Release();
			rootSig->Release();
			vbRes->Release();
			ibRes->Release();
			cbRes1->Release();
			cbRes2->Release();
			srvHeap->Release();
			tex->Release();
			uploadHeap->Release();
		}

	private:
		ID3D12PipelineState*	pso;
		Memory					vs_mem;
		Memory					ps_mem;
		ID3D12RootSignature*	rootSig;

		ID3D12Resource*			vbRes;
		ID3D12Resource*			ibRes;
		D3D12_VERTEX_BUFFER_VIEW	vbView;
		D3D12_INDEX_BUFFER_VIEW		ibView;

		ID3D12Resource*			cbRes1;
		ID3D12Resource*			cbRes2;

		ID3D12DescriptorHeap*	srvHeap;
		ID3D12Resource*			tex;
		ID3D12Resource*			uploadHeap;

		D3D12_VIEWPORT			viewports[1];
		D3D12_RECT				scissorRects[1];

		Mesh					mesh;
	};


	wchar_t g_WindowClassName[] = L"D3D12_Study";

	LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		switch (msg)
		{
		case WM_SIZE:
		{
			Application* app = reinterpret_cast<Application*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));
			if (nullptr != app)
				app->OnResize(static_cast<UINT>(lParam & 0xffffU), static_cast<UINT>(lParam >> 16U));
		}
		break;
		case WM_DESTROY:
			PostQuitMessage(0);
			break;
		default:
			break;
		}

		return DefWindowProc(hWnd, msg, wParam, lParam);
	}

}


int CALLBACK WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	Application app;

	WNDCLASSEX cls = {};
	cls.cbSize = sizeof(WNDCLASSEX);
	cls.hbrBackground = (HBRUSH)COLOR_BACKGROUND;
	cls.hInstance = hInstance;
	cls.lpfnWndProc = WndProc;
	cls.lpszClassName = g_WindowClassName;
	cls.style = CS_HREDRAW | CS_VREDRAW;

	if (!RegisterClassEx(&cls))
	{
		return -1;
	}

	DWORD winStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

	RECT windowRect = { 0, 0, 800, 600 };
	AdjustWindowRect(&windowRect, winStyle, FALSE);

	RECT screenRect;
	GetClientRect(GetDesktopWindow(), &screenRect);
	int windowX = screenRect.right / 2 - (windowRect.right - windowRect.left) / 2;
	int windowY = screenRect.bottom / 2 - (windowRect.bottom - windowRect.top) / 2;

	HWND hWnd = CreateWindow(
		g_WindowClassName,
		g_WindowClassName,
		winStyle,
		windowX, windowY,
		windowRect.right - windowRect.left,
		windowRect.bottom - windowRect.top,
		nullptr,
		nullptr,
		hInstance,
		reinterpret_cast<void*>(&app));

	if (!hWnd)
	{
		UnregisterClass(g_WindowClassName, hInstance);
		return -1;
	}

	if (!app.Init(hWnd, 800, 600))
	{
		return -1;
	}

	ShowWindow(hWnd, SW_SHOW);

	bool running = true;
	MSG msg;
	while (running)
	{
		while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			if (msg.message == WM_QUIT)
			{
				running = false;
				break;
			}
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}

		app.Update();
	}

	app.Release();

	return 0;
}
