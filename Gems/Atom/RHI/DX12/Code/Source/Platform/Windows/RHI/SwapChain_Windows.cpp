/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */
#include <RHI/SwapChain.h>
#include <RHI/Device.h>
#include <RHI/Conversions.h>
#include <RHI/NsightAftermath.h>

namespace AZ
{
    namespace DX12
    {
        RHI::ResultCode SwapChain::InitInternal(RHI::Device& deviceBase, const RHI::SwapChainDescriptor& descriptor, RHI::SwapChainDimensions* nativeDimensions)
        {
            // Check whether tearing support is available for full screen borderless windowed mode.
            BOOL allowTearing = FALSE;
            DX12Ptr<IDXGIFactoryX> dxgiFactory;
            DX12::AssertSuccess(CreateDXGIFactory2(0, IID_GRAPHICS_PPV_ARGS(dxgiFactory.GetAddressOf())));
            m_isTearingSupported = SUCCEEDED(dxgiFactory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing))) && allowTearing;

            if (nativeDimensions)
            {
                *nativeDimensions = descriptor.m_dimensions;
            }
            const uint32_t SwapBufferCount = RHI::Limits::Device::FrameCountMax;

            DXGI_SWAP_CHAIN_DESCX swapChainDesc = {};
            swapChainDesc.SampleDesc.Quality = 0;
            swapChainDesc.SampleDesc.Count = 1;
            swapChainDesc.BufferCount = SwapBufferCount;
            swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            swapChainDesc.Width = descriptor.m_dimensions.m_imageWidth;
            swapChainDesc.Height = descriptor.m_dimensions.m_imageHeight;
            swapChainDesc.Format = ConvertFormat(descriptor.m_dimensions.m_imageFormat);
            swapChainDesc.Scaling = DXGI_SCALING_NONE;
            swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
            if (m_isTearingSupported)
            {
                // It is recommended to always use the tearing flag when it is available.
                swapChainDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
            }

            IUnknown* window = reinterpret_cast<IUnknown*>(descriptor.m_window.GetIndex());
            RHI::ResultCode result = static_cast<Device&>(deviceBase).CreateSwapChain(reinterpret_cast<IUnknown*>(descriptor.m_window.GetIndex()), swapChainDesc, m_swapChain);
            if (result == RHI::ResultCode::Success)
            {
                ConfigureDisplayMode(*nativeDimensions);

                // According to various docs (and the D3D12Fulscreen sample), when tearing is supported
                // a borderless full screen window is always preferred over exclusive full screen mode.
                //
                // - https://devblogs.microsoft.com/directx/demystifying-full-screen-optimizations/
                // - https://docs.microsoft.com/en-us/windows/win32/direct3ddxgi/variable-refresh-rate-displays
                //
                // So we have modelled our full screen support on the D3D12Fulscreen sample by choosing
                // the best full screen mode to use based on whether tearing is supported by the device.
                //
                // It would be possible to allow a choice between these different full screen modes,
                // but we have chosen not to given that guidance for DX12 appears to be discouraging
                // the use of exclusive full screen mode, and because no other platforms support it.
                if (m_isTearingSupported)
                {
                    // To use tearing in full screen Win32 apps the application should present to a fullscreen borderless window and disable automatic
                    // ALT+ENTER fullscreen switching using IDXGIFactory::MakeWindowAssociation (see also implementation of SwapChain::PresentInternal).
                    // You must call the MakeWindowAssociation method after the creation of the swap chain, and on the factory object associated with the
                    // target HWND swap chain, which you can guarantee by calling the IDXGIObject::GetParent method on the swap chain to locate the factory.
                    //
                    // ToDo: ATOM-14673 We should handle ALT+ENTER in the windows message loop and call AzFramework::NativeWindow::ToggleFullScreenState in
                    // response, but that will have to wait until the WndProc function moves out of CrySystem (ideally into AzFramework::ApplicationWindows).
                    IDXGIFactoryX* parentFactory = nullptr;
                    m_swapChain->GetParent(__uuidof(IDXGIFactoryX), (void **)&parentFactory);
                    DX12::AssertSuccess(parentFactory->MakeWindowAssociation(reinterpret_cast<HWND>(window), DXGI_MWA_NO_ALT_ENTER));
                }
            }
            return result;
        }

        void SwapChain::ShutdownInternal()
        {
            // We must exit exclusive full screen mode before shutting down.
            // Safe to call even if not in the exclusive full screen state.
            m_swapChain->SetFullscreenState(0, nullptr);
            m_swapChain = nullptr;
        }

        uint32_t SwapChain::PresentInternal()
        {
            if (m_swapChain)
            {
                // It is recommended to always pass the DXGI_PRESENT_ALLOW_TEARING flag when it is supported, even when presenting in windowed mode.
                // But it cannot be used in an application that is currently in full screen exclusive mode, set by calling SetFullscreenState(TRUE).
                // To use this flag in full screen Win32 apps the application should present to a fullscreen borderless window and disable automatic
                // ALT+ENTER fullscreen switching using IDXGIFactory::MakeWindowAssociation (please see implementation of SwapChain::InitInternal).
                // UINT presentFlags = (m_isTearingSupported && !m_isInFullScreenExclusiveState) ? DXGI_PRESENT_ALLOW_TEARING : 0;
                HRESULT hresult = m_swapChain->Present(GetDescriptor().m_verticalSyncInterval, 0);

                if (hresult == DXGI_ERROR_DEVICE_REMOVED)
                {
                    HRESULT deviceRemovedResult = GetDevice().GetDevice()->GetDeviceRemovedReason();
                    switch (deviceRemovedResult)
                    {
                    case DXGI_ERROR_DEVICE_HUNG:
                        AZ_TracePrintf(
                            "DX12",
                            "DXGI_ERROR_DEVICE_HUNG - The application's device failed due to badly formed commands sent by the "
                            "application. This is an design-time issue that should be investigated and fixed.");
                        break;
                    case DXGI_ERROR_DEVICE_REMOVED:
                        AZ_TracePrintf(
                            "DX12",
                            "DXGI_ERROR_DEVICE_REMOVED - The video card has been physically removed from the system, or a driver upgrade "
                            "for the video card has occurred. The application should destroy and recreate the device. For help debugging "
                            "the problem, call ID3D10Device::GetDeviceRemovedReason.");
                        break;
                    case DXGI_ERROR_DEVICE_RESET:
                        AZ_TracePrintf(
                            "DX12",
                            "DXGI_ERROR_DEVICE_RESET - The device failed due to a badly formed command. This is a run-time issue; The "
                            "application should destroy and recreate the device.");
                        break;
                    case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
                        AZ_TracePrintf(
                            "DX12",
                            "DXGI_ERROR_DRIVER_INTERNAL_ERROR - The driver encountered a problem and was put into the device removed "
                            "state.");
                        break;
                    case DXGI_ERROR_INVALID_CALL:
                        AZ_TracePrintf(
                            "DX12",
                            "DXGI_ERROR_INVALID_CALL - The application provided invalid parameter data; this must be debugged and fixed "
                            "before the application is released.");
                        break;
                    case DXGI_ERROR_ACCESS_DENIED:
                        AZ_TracePrintf(
                            "DX12",
                            "DXGI_ERROR_ACCESS_DENIED - You tried to use a resource to which you did not have the required access "
                            "privileges. This error is most typically caused when you write to a shared resource with read-only access.");
                        break;
                    case S_OK:
                        AZ_TracePrintf("DX12", "S_OK - The method succeeded without an error.");
                        break;
                    }

                    if (GetDevice().IsAftermathInitialized())
                    {
                        // DXGI_ERROR error notification is asynchronous to the NVIDIA display
                        // driver's GPU crash handling. Give the Nsight Aftermath GPU crash dump
                        // thread some time to do its work before terminating the process.
                        Sleep(3000);

                        // Try outputting the name of the last scope that was executing on the GPU
                        // There is a good chance that is the cause of the GPU crash and should be investigated first
                        Aftermath::OutputLastScopeExecutingOnGPU(GetDevice().GetAftermathGPUCrashTracker());
                    }
                }

                return (GetCurrentImageIndex() + 1) % GetImageCount();
            }

            return GetCurrentImageIndex();
        }

        RHI::ResultCode SwapChain::ResizeInternal(const RHI::SwapChainDimensions& dimensions, RHI::SwapChainDimensions* nativeDimensions)
        {
            GetDevice().WaitForIdle();

            DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
             m_swapChain->GetDesc(&swapChainDesc);
            if (AssertSuccess(m_swapChain->ResizeBuffers(
                    dimensions.m_imageCount,
                    dimensions.m_imageWidth,
                    dimensions.m_imageHeight,
                    swapChainDesc.BufferDesc.Format,
                    swapChainDesc.Flags)))
            {
                if (nativeDimensions)
                {
                    *nativeDimensions = dimensions;
                }
                ConfigureDisplayMode(dimensions);

                // Check whether SetFullscreenState was used to enter full screen exclusive state.
                BOOL fullscreenState;
                m_isInFullScreenExclusiveState = SUCCEEDED(m_swapChain->GetFullscreenState(&fullscreenState, nullptr)) && fullscreenState;

                return RHI::ResultCode::Success;
            }

            return RHI::ResultCode::Fail;
        }

        bool SwapChain::IsExclusiveFullScreenPreferred() const
        {
            return !m_isTearingSupported;
        }

        bool SwapChain::GetExclusiveFullScreenState() const
        {
            return m_isInFullScreenExclusiveState;
        }

        bool SwapChain::SetExclusiveFullScreenState(bool fullScreenState)
        {
            if (m_swapChain)
            {
                m_swapChain->SetFullscreenState(fullScreenState, nullptr);
            }

            // The above call to SetFullscreenState will ultimately result in
            // SwapChain:ResizeInternal being called above where we set this.
            return (fullScreenState == m_isInFullScreenExclusiveState);
        }

        void SwapChain::ConfigureDisplayMode(const RHI::SwapChainDimensions& dimensions)
        {
            DXGI_COLOR_SPACE_TYPE colorSpace = static_cast<DXGI_COLOR_SPACE_TYPE>(InvalidColorSpace);
            bool hdrEnabled = false;

            if (dimensions.m_imageFormat == RHI::Format::R8G8B8A8_UNORM)
            {
                colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
            }
            else if (dimensions.m_imageFormat == RHI::Format::R10G10B10A2_UNORM)
            {
                colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                hdrEnabled = true;
            }
            else
            {
                AZ_Assert(false, "Unhandled swapchain buffer format.");
            }

            if (m_colorSpace != colorSpace)
            {
                EnsureColorSpace(colorSpace);
                if (hdrEnabled)
                {
                    // [GFX TODO][ATOM-2587] How to specify and determine the limits of the display and scene?
                    float maxOutputNits = 1000.f;
                    float minOutputNits = 0.001f;
                    float maxContentLightLevelNits = 2000.f;
                    float maxFrameAverageLightLevelNits = 500.f;
                    SetHDRMetaData(maxOutputNits, minOutputNits, maxContentLightLevelNits, maxFrameAverageLightLevelNits);
                }
                else
                {
                    DisableHdr();
                }
            }
        }

        void SwapChain::EnsureColorSpace(const DXGI_COLOR_SPACE_TYPE& colorSpace)
        {
            AZ_Assert(colorSpace != DXGI_COLOR_SPACE_CUSTOM, "Invalid color space type for swapchain.");

            UINT colorSpaceSupport = 0;
            HRESULT res = m_swapChain->CheckColorSpaceSupport(colorSpace, &colorSpaceSupport);
            if (res == S_OK)
            {
                if (colorSpaceSupport & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)
                {
                    [[maybe_unused]] HRESULT hr = m_swapChain->SetColorSpace1(colorSpace);
                    AZ_Assert(S_OK == hr, "Failed to set swap chain color space.");
                    m_colorSpace = colorSpace;
                }
            }
        }

        void SwapChain::DisableHdr()
        {
            // Reset the HDR metadata.
            [[maybe_unused]] HRESULT hr = m_swapChain->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_NONE, 0, nullptr);
            AZ_Assert(S_OK == hr, "Failed to reset HDR metadata.");
        }

        void SwapChain::SetHDRMetaData(float maxOutputNits, float minOutputNits, float maxContentLightLevelNits, float maxFrameAverageLightLevelNits)
        {
            struct DisplayChromacities
            {
                float RedX;
                float RedY;
                float GreenX;
                float GreenY;
                float BlueX;
                float BlueY;
                float WhiteX;
                float WhiteY;
            };
            static const DisplayChromacities DisplayChromacityList[] =
            {
                { 0.64000f, 0.33000f, 0.30000f, 0.60000f, 0.15000f, 0.06000f, 0.31270f, 0.32900f }, // Display Gamut Rec709 
                { 0.70800f, 0.29200f, 0.17000f, 0.79700f, 0.13100f, 0.04600f, 0.31270f, 0.32900f }, // Display Gamut Rec2020
            };

            // Select the chromaticity based on HDR format of the DWM.
            int selectedChroma = 0;
            if (m_colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709)
            {
                selectedChroma = 0;
            }
            else if (m_colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
            {
                selectedChroma = 1;
            }
            else
            {
                AZ_Assert(false, "Unhandled color space for swapchain.");
            }

            // These are scaling factors that the api expects values to be normalized to.
            const float ChromaticityScalingFactor = 50000.0f;
            const float LuminanceScalingFactor = 10000.0f;

            const DisplayChromacities& Chroma = DisplayChromacityList[selectedChroma];
            DXGI_HDR_METADATA_HDR10 HDR10MetaData = {};
            HDR10MetaData.RedPrimary[0] = static_cast<UINT16>(Chroma.RedX * ChromaticityScalingFactor);
            HDR10MetaData.RedPrimary[1] = static_cast<UINT16>(Chroma.RedY * ChromaticityScalingFactor);
            HDR10MetaData.GreenPrimary[0] = static_cast<UINT16>(Chroma.GreenX * ChromaticityScalingFactor);
            HDR10MetaData.GreenPrimary[1] = static_cast<UINT16>(Chroma.GreenY * ChromaticityScalingFactor);
            HDR10MetaData.BluePrimary[0] = static_cast<UINT16>(Chroma.BlueX * ChromaticityScalingFactor);
            HDR10MetaData.BluePrimary[1] = static_cast<UINT16>(Chroma.BlueY * ChromaticityScalingFactor);
            HDR10MetaData.WhitePoint[0] = static_cast<UINT16>(Chroma.WhiteX * ChromaticityScalingFactor);
            HDR10MetaData.WhitePoint[1] = static_cast<UINT16>(Chroma.WhiteY * ChromaticityScalingFactor);
            HDR10MetaData.MaxMasteringLuminance = static_cast<UINT>(maxOutputNits * LuminanceScalingFactor);
            HDR10MetaData.MinMasteringLuminance = static_cast<UINT>(minOutputNits * LuminanceScalingFactor);
            HDR10MetaData.MaxContentLightLevel = static_cast<UINT16>(maxContentLightLevelNits);
            HDR10MetaData.MaxFrameAverageLightLevel = static_cast<UINT16>(maxFrameAverageLightLevelNits);
            [[maybe_unused]] HRESULT hr = m_swapChain->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(DXGI_HDR_METADATA_HDR10), &HDR10MetaData);
            AZ_Assert(S_OK == hr, "Failed to set HDR meta data.");
        }
    }
}
