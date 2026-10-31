// ===========================================================================
// BmlLegacyOverlay.cpp - Dear ImGui overlay host for the original BML.
// ---------------------------------------------------------------------------
// The original loader exports no ImGui symbol at all, so this file owns the
// whole stack:
//   * one ImGui context, opened in OnLoad and destroyed in OnUnload;
//   * a Virtools renderer backend: the draw data is submitted with
//     CKRenderContext::DrawPrimitive from a post-sprite callback, i.e. inside
//     the engine's frame, after the game sprites (same slot BML+ uses);
//   * the frame itself: NewFrame comes from OverlayHost::BeginFrame (called by
//     the shared Overlay.cpp before drawing), Render from EndFrame.
//
// Rendering technique follows BML+'s own imgui_impl_ck2 backend: pixels are
// pre-transformed screen coordinates (CKRST_DP_CL_VCT), alpha blended, no depth
// test, one CKTexture for the ImGui font atlas.  The code is written for the
// Virtools 2.1 SDK headers, not BML's trimmed copies, because the drawing
// methods are virtual and Ballance's CK2.dll does not export them by name.
// ===========================================================================
#include "bml-old/BmlLegacyOverlay.h"

#include "viz/OverlayHost.h"

#include <BML/IBML.h>

#include <CKAll.h>

#include <imgui.h>

#include <cstring>
#include <vector>

namespace {

IBML *g_BML = nullptr;
CKContext *g_CK = nullptr;
CKRenderContext *g_Render = nullptr;
ImGuiContext *g_Context = nullptr;
ImGuiContext *g_Previous = nullptr;
ImDrawData *g_Pending = nullptr;
bool g_CallbackRegistered = false;

struct BackendData {
    CKContext *Context = nullptr;
    CKRenderContext *RenderContext = nullptr;
    std::vector<CKTexture *> Textures;
};

BackendData *GetBackend() {
    if (!g_Context) {
        return nullptr;
    }
    return static_cast<BackendData *>(ImGui::GetIO().BackendRendererUserData);
}

// ---------------------------------------------------------------------------
// Font atlas texture management (ImGui 1.92 texture API)
// ---------------------------------------------------------------------------
void CopyTextureRegion(const ImTextureData *tex, const ImU32 *src, int srcPitch, ImU32 *dst,
                       int dstPitch, int width, int height) {
    (void) tex;
    for (int y = 0; y < height; ++y) {
        const auto *srcRow = reinterpret_cast<const unsigned char *>(src) + static_cast<size_t>(srcPitch) * y;
        auto *dstRow = reinterpret_cast<unsigned char *>(dst) + static_cast<size_t>(dstPitch) * y;
        std::memcpy(dstRow, srcRow, static_cast<size_t>(width) * 4);
    }
}

void DestroyTexture(CKTexture *texture) {
    if (!texture) {
        return;
    }
    BackendData *backend = GetBackend();
    if (backend) {
        for (size_t i = 0; i < backend->Textures.size(); ++i) {
            if (backend->Textures[i] == texture) {
                backend->Textures.erase(backend->Textures.begin() + static_cast<ptrdiff_t>(i));
                break;
            }
        }
    }
    if (g_CK) {
        g_CK->DestroyObject(texture);
    }
}

void UpdateTexture(ImTextureData *tex) {
    BackendData *backend = GetBackend();
    if (!backend) {
        return;
    }

    if (tex->Status == ImTextureStatus_WantCreate) {
        auto *texture = static_cast<CKTexture *>(
                backend->Context->CreateObject(CKCID_TEXTURE, (CKSTRING) "FlexGravityOverlay"));
        if (!texture) {
            return;
        }
        texture->ModifyObjectFlags(CK_OBJECT_NOTTOBESAVED | CK_OBJECT_NOTTOBEDELETED, 0);
        if (!texture->Create(tex->Width, tex->Height)) {
            backend->Context->DestroyObject(texture);
            return;
        }
        CKBYTE *surface = texture->LockSurfacePtr();
        if (!surface) {
            backend->Context->DestroyObject(texture);
            return;
        }
        CopyTextureRegion(tex, static_cast<const ImU32 *>(tex->GetPixels()), tex->Width * 4,
                          reinterpret_cast<ImU32 *>(surface), tex->Width * 4, tex->Width, tex->Height);
        texture->ReleaseSurfacePtr();
        texture->SetDesiredVideoFormat(_32_ARGB8888);
        if (!texture->SystemToVideoMemory(backend->RenderContext, TRUE)) {
            backend->Context->DestroyObject(texture);
            return;
        }
        tex->SetTexID(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(texture)));
        tex->SetStatus(ImTextureStatus_OK);
        backend->Textures.push_back(texture);
        return;
    }

    if (tex->Status == ImTextureStatus_WantUpdates) {
        auto *texture = reinterpret_cast<CKTexture *>(static_cast<intptr_t>(tex->TexID));
        if (!texture) {
            return;
        }
        CKBYTE *surface = texture->LockSurfacePtr();
        if (!surface) {
            return;
        }
        for (const ImTextureRect &rect : tex->Updates) {
            const auto *src = static_cast<const ImU32 *>(tex->GetPixelsAt(rect.x, rect.y));
            auto *dst = reinterpret_cast<ImU32 *>(surface) + rect.x + rect.y * tex->Width;
            CopyTextureRegion(tex, src, tex->Width * 4, dst, tex->Width * 4, rect.w, rect.h);
        }
        texture->ReleaseSurfacePtr();
        if (!texture->SystemToVideoMemory(backend->RenderContext, TRUE)) {
            return;
        }
        tex->SetStatus(ImTextureStatus_OK);
        return;
    }

    if (tex->Status == ImTextureStatus_WantDestroy) {
        auto *texture = reinterpret_cast<CKTexture *>(static_cast<intptr_t>(tex->TexID));
        if (texture) {
            DestroyTexture(texture);
        }
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
    }
}

// ---------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------
void SetupRenderState(ImDrawData *drawData) {
    BackendData *backend = GetBackend();
    if (!backend || !backend->RenderContext) {
        return;
    }
    CKRenderContext *dev = backend->RenderContext;

    VxRect viewport(0.0f, 0.0f, drawData->DisplaySize.x, drawData->DisplaySize.y);
    dev->SetViewRect(viewport);

    dev->SetState(VXRENDERSTATE_FILLMODE, VXFILL_SOLID);
    dev->SetState(VXRENDERSTATE_SHADEMODE, VXSHADE_GOURAUD);
    dev->SetState(VXRENDERSTATE_CULLMODE, VXCULL_NONE);
    dev->SetState(VXRENDERSTATE_WRAP0, 0);
    dev->SetState(VXRENDERSTATE_SRCBLEND, VXBLEND_SRCALPHA);
    dev->SetState(VXRENDERSTATE_DESTBLEND, VXBLEND_INVSRCALPHA);
    dev->SetState(VXRENDERSTATE_ALPHATESTENABLE, FALSE);
    dev->SetState(VXRENDERSTATE_ZWRITEENABLE, FALSE);
    dev->SetState(VXRENDERSTATE_ZENABLE, FALSE);
    dev->SetState(VXRENDERSTATE_ALPHABLENDENABLE, TRUE);
    dev->SetState(VXRENDERSTATE_BLENDOP, VXBLENDOP_ADD);
    dev->SetState(VXRENDERSTATE_FOGENABLE, FALSE);
    dev->SetState(VXRENDERSTATE_SPECULARENABLE, FALSE);
    dev->SetState(VXRENDERSTATE_STENCILENABLE, FALSE);
    dev->SetState(VXRENDERSTATE_CLIPPING, TRUE);
    dev->SetState(VXRENDERSTATE_LIGHTING, FALSE);

    dev->SetTextureStageState(CKRST_TSS_ADDRESS, VXTEXTURE_ADDRESSCLAMP);
    dev->SetTextureStageState(CKRST_TSS_TEXTUREMAPBLEND, VXTEXTUREBLEND_MODULATEALPHA);
    dev->SetTextureStageState(CKRST_TSS_STAGEBLEND, 0, 1);
    dev->SetTextureStageState(CKRST_TSS_MINFILTER, VXTEXTUREFILTER_LINEAR);
    dev->SetTextureStageState(CKRST_TSS_MAGFILTER, VXTEXTUREFILTER_LINEAR);
}

void RenderDrawData(ImDrawData *drawData) {
    if (!drawData) {
        return;
    }

    const int framebufferWidth =
            static_cast<int>(drawData->DisplaySize.x * drawData->FramebufferScale.x);
    const int framebufferHeight =
            static_cast<int>(drawData->DisplaySize.y * drawData->FramebufferScale.y);
    if (framebufferWidth <= 0 || framebufferHeight <= 0) {
        return;
    }

    BackendData *backend = GetBackend();
    if (!backend || !backend->RenderContext) {
        return;
    }
    CKRenderContext *dev = backend->RenderContext;

    // Catch up with the texture requests queued by this frame (font atlas).
    if (drawData->Textures) {
        for (ImTextureData *tex : *drawData->Textures) {
            if (tex->Status != ImTextureStatus_OK) {
                UpdateTexture(tex);
            }
        }
    }

    SetupRenderState(drawData);

    const ImVec2 clipOffset = drawData->DisplayPos;
    const ImVec2 clipScale = drawData->FramebufferScale;

    for (int listIndex = 0; listIndex < drawData->CmdListsCount; ++listIndex) {
        const ImDrawList *cmdList = drawData->CmdLists[listIndex];
        const ImDrawVert *vertexBuffer = cmdList->VtxBuffer.Data;
        const ImDrawIdx *indexBuffer = cmdList->IdxBuffer.Data;

        VxDrawPrimitiveData *data = nullptr;
        const bool largeMesh = cmdList->VtxBuffer.Size >= 0xFFFF;

        if (!largeMesh) {
            data = dev->GetDrawPrimitiveStructure(
                    static_cast<CKRST_DPFLAGS>(CKRST_DP_CL_VCT | CKRST_DP_VBUFFER),
                    cmdList->VtxBuffer.Size);
            if (!data) {
                data = dev->GetDrawPrimitiveStructure(CKRST_DP_CL_VCT, cmdList->VtxBuffer.Size);
            }
            if (!data) {
                continue;
            }
            XPtrStrided<VxVector4> positions(data->PositionPtr, data->PositionStride);
            XPtrStrided<CKDWORD> colors(data->ColorPtr, data->ColorStride);
            XPtrStrided<VxUV> uvs(data->TexCoordPtr, data->TexCoordStride);
            for (int i = 0; i < cmdList->VtxBuffer.Size; ++i) {
                positions->Set(vertexBuffer[i].pos.x, vertexBuffer[i].pos.y, 0.0f, 1.0f);
                *colors = static_cast<CKDWORD>(vertexBuffer[i].col);
                uvs->u = vertexBuffer[i].uv.x;
                uvs->v = vertexBuffer[i].uv.y;
                ++positions;
                ++colors;
                ++uvs;
            }
        }

        for (int cmdIndex = 0; cmdIndex < cmdList->CmdBuffer.Size; ++cmdIndex) {
            const ImDrawCmd *cmd = &cmdList->CmdBuffer[cmdIndex];

            if (cmd->UserCallback) {
                if (cmd->UserCallback == ImDrawCallback_ResetRenderState) {
                    SetupRenderState(drawData);
                } else {
                    cmd->UserCallback(cmdList, cmd);
                }
                continue;
            }

            ImVec2 clipMin((cmd->ClipRect.x - clipOffset.x) * clipScale.x,
                           (cmd->ClipRect.y - clipOffset.y) * clipScale.y);
            ImVec2 clipMax((cmd->ClipRect.z - clipOffset.x) * clipScale.x,
                           (cmd->ClipRect.w - clipOffset.y) * clipScale.y);
            if (clipMin.x < 0.0f) {
                clipMin.x = 0.0f;
            }
            if (clipMin.y < 0.0f) {
                clipMin.y = 0.0f;
            }
            if (clipMax.x > static_cast<float>(framebufferWidth)) {
                clipMax.x = static_cast<float>(framebufferWidth);
            }
            if (clipMax.y > static_cast<float>(framebufferHeight)) {
                clipMax.y = static_cast<float>(framebufferHeight);
            }
            if (clipMax.x <= clipMin.x || clipMax.y <= clipMin.y) {
                continue;
            }

            if (largeMesh) {
                const int vertexCount = cmd->ElemCount;
                data = dev->GetDrawPrimitiveStructure(
                        static_cast<CKRST_DPFLAGS>(CKRST_DP_CL_VCT | CKRST_DP_VBUFFER),
                        vertexCount);
                if (!data) {
                    data = dev->GetDrawPrimitiveStructure(CKRST_DP_CL_VCT, vertexCount);
                }
                if (!data) {
                    continue;
                }
                XPtrStrided<VxVector4> positions(data->PositionPtr, data->PositionStride);
                XPtrStrided<CKDWORD> colors(data->ColorPtr, data->ColorStride);
                XPtrStrided<VxUV> uvs(data->TexCoordPtr, data->TexCoordStride);
                const ImDrawVert *source = vertexBuffer + cmd->VtxOffset;
                for (int i = 0; i < vertexCount; ++i) {
                    positions->Set(source[i].pos.x, source[i].pos.y, 0.0f, 1.0f);
                    *colors = static_cast<CKDWORD>(source[i].col);
                    uvs->u = source[i].uv.x;
                    uvs->v = source[i].uv.y;
                    ++positions;
                    ++colors;
                    ++uvs;
                }
            }

            auto *object = reinterpret_cast<CKObject *>(static_cast<intptr_t>(cmd->GetTexID()));
            if (!object) {
                continue;
            }
            if (object->GetClassID() == CKCID_TEXTURE) {
                dev->SetTexture(static_cast<CKTexture *>(object));
                dev->DrawPrimitive(VX_TRIANGLELIST,
                                   const_cast<CKWORD *>(indexBuffer + cmd->IdxOffset),
                                   static_cast<int>(cmd->ElemCount), data);
            }
        }
    }
}

void PostSpriteRender(CKRenderContext * /*dev*/, void * /*argument*/) {
    if (!g_Pending) {
        return;
    }
    ImDrawData *pending = g_Pending;
    g_Pending = nullptr;
    RenderDrawData(pending);
}

bool FlushPendingFrame() {
    if (!g_Pending) {
        return false;
    }
    ImDrawData *pending = g_Pending;
    g_Pending = nullptr;
    RenderDrawData(pending);
    return true;
}

// ---------------------------------------------------------------------------
// Context lifecycle
// ---------------------------------------------------------------------------
bool EnsureContext() {
    if (g_Context) {
        return true;
    }
    if (!g_BML) {
        return false;
    }
    g_CK = g_BML->GetCKContext();
    g_Render = g_BML->GetRenderContext();
    if (!g_CK || !g_Render) {
        return false;
    }

    IMGUI_CHECKVERSION();
    g_Context = ImGui::CreateContext();
    if (!g_Context) {
        return false;
    }

    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.BackendRendererName = "flexgravity_ck2";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

    auto *backend = IM_NEW(BackendData)();
    backend->Context = g_CK;
    backend->RenderContext = g_Render;
    io.BackendRendererUserData = backend;

    g_Render->AddPostSpriteRenderCallBack(&PostSpriteRender, nullptr);
    g_CallbackRegistered = true;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Public lifecycle used by the mod
// ---------------------------------------------------------------------------
void BmlLegacyOverlayInitialize(IBML *bml) {
    g_BML = bml;
    EnsureContext();
}

bool BmlLegacyOverlayFlushPending() { return FlushPendingFrame(); }

void BmlLegacyOverlayShutdown() {    if (g_Render && g_CallbackRegistered) {
        g_Render->RemovePostSpriteRenderCallBack(&PostSpriteRender, nullptr);
        g_CallbackRegistered = false;
    }

    if (g_Context) {
        BackendData *backend = GetBackend();
        if (backend) {
            for (CKTexture *texture : backend->Textures) {
                if (texture && g_CK) {
                    g_CK->DestroyObject(texture);
                }
            }
            backend->Textures.clear();
            IM_DELETE(backend);
        }
        ImGui::DestroyContext(g_Context);
        g_Context = nullptr;
    }

    g_Pending = nullptr;
    g_CK = nullptr;
    g_Render = nullptr;
    g_BML = nullptr;
    g_Previous = nullptr;
}

// ---------------------------------------------------------------------------
// OverlayHost: the interface the shared overlay code talks to
// ---------------------------------------------------------------------------
namespace OverlayHost {

bool HasContext() { return EnsureContext(); }

bool BeginFrame() {
    if (!EnsureContext()) {
        return false;
    }
    const int width = g_Render ? g_Render->GetWidth() : 0;
    const int height = g_Render ? g_Render->GetHeight() : 0;
    if (width <= 0 || height <= 0) {
        return false;
    }
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    io.DeltaTime = 1.0f / 60.0f;

    // ImGui's built-in 13 px font is much smaller than the text BML+ shows in
    // its overlay at the resolutions Ballance runs at.  Scale it with the
    // viewport instead of hard-coding a size: a 60th of the height gives 20 px
    // at 1600x1200, and the floor keeps small windows readable.
    float fontSize = static_cast<float>(height) / 45.0f;
    if (fontSize < 13.0f) {
        fontSize = 13.0f;
    }
    if (fontSize > 40.0f) {
        fontSize = 40.0f;
    }
    ImGui::GetStyle().FontSizeBase = fontSize;

    ImGui::NewFrame();
    return true;
}

void EndFrame() {
    if (!g_Context) {
        return;
    }
    ImGui::Render();
    g_Pending = ImGui::GetDrawData();
}

void *CurrentContext() { return static_cast<void *>(g_Context); }

void EnterContext() {
    g_Previous = ImGui::GetCurrentContext();
    if (g_Context) {
        ImGui::SetCurrentContext(g_Context);
    }
}

void LeaveContext() {
    if (g_Context) {
        ImGui::SetCurrentContext(g_Previous);
    }
}

} // namespace OverlayHost
