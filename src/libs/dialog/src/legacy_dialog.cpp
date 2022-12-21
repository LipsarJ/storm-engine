#include "legacy_dialog.hpp"

#include "dialog.hpp"

#include <animation.h>
#include <core.h>
#include <geometry.h>
#include <math_inlines.h>
#include <model.h>
#include <shared/messages.h>
#include <string_compare.hpp>
#include <vma.hpp>

#include <array>

CREATE_CLASS(LegacyDialog)

namespace
{

constexpr std::string_view DIALOG_INI_FILE_PATH = "Resource/Ini/dialog.ini";
const char *DEFAULT_INTERFACE_TEXTURE = "dialog/dialog.tga";

constexpr const uint32_t COLOR_NORMAL = 0xFFFFFFFF;
constexpr const uint32_t COLOR_LINK_UNSELECTED = ARGB(255, 127, 127, 127);

constexpr const uint32_t UNFADE_TIME = 1000;

int32_t LoadFont(const std::string_view &fontName, INIFILE &ini, VDX9RENDER &renderService)
{
    std::array<char, MAX_PATH> string_buffer{};
    ini.ReadString("DIALOG", fontName.data(), string_buffer.data(), string_buffer.size(), "DIALOG0");
    return renderService.LoadFont(string_buffer.data());
}

void FillIndexBuffer(VDX9RENDER &renderService, int32_t indexBuffer, size_t spriteCount)
{
    auto *pI = static_cast<uint16_t *>(renderService.LockIndexBuffer(indexBuffer));
    for (size_t n = 0; n < spriteCount; n++)
    {
        pI[n * 6 + 0] = static_cast<uint16_t>(n * 4 + 0);
        pI[n * 6 + 1] = static_cast<uint16_t>(n * 4 + 2);
        pI[n * 6 + 2] = static_cast<uint16_t>(n * 4 + 1);
        pI[n * 6 + 3] = static_cast<uint16_t>(n * 4 + 1);
        pI[n * 6 + 4] = static_cast<uint16_t>(n * 4 + 2);
        pI[n * 6 + 5] = static_cast<uint16_t>(n * 4 + 3);
    }
    renderService.UnLockIndexBuffer(indexBuffer);
}

constexpr FRECT ScaleUv(FRECT uv)
{
    constexpr float hScale = 1.f / 1024.f;
    constexpr float vScale = 1.f / 256.f;

    return {
        uv.x1 * hScale,
        uv.y1 * vScale,
        uv.x2 * hScale,
        uv.y2 * vScale,
    };
}

struct SpriteInfo
{
    FRECT position{};
    FRECT uv{};
};

constexpr const size_t DIALOG_MAX_LINES = 8;
constexpr const float DIVIDER_HEIGHT = 10;
constexpr const int32_t DIALOG_LINE_HEIGHT = 26;

constexpr std::array SPRITE_DATA = {
    // Head overlay
    SpriteInfo{
        {29, 25, 147, 37},
        ScaleUv({904, 91, 904 + 119, 91 + 12}),
    },
    SpriteInfo{
        .position = {29, 173, 146, 185},
        .uv = ScaleUv({904, 105, 904 + 119, 105 + 11}),
    },
    // General
    SpriteInfo{
        .position = {-39, -39, 169, 216},
        .uv = ScaleUv({0, 0, 208, 255}),
    },
    SpriteInfo{
        .position = {169, -39, 678, 79},
        .uv = ScaleUv({208, 0, 757, 118}),
    },
    SpriteInfo{
        .position = {-39, 451, 678, 518},
        .uv = ScaleUv({209, 189, 1023, 255}),
    },
    // Static vertices 2
    SpriteInfo{
        .position = {29, 25, 147, 37},
        .uv = ScaleUv({904, 91, 1023, 103}),
    },
    SpriteInfo{
        .position = {29, 173, 146, 185},
        .uv = ScaleUv({904, 105, 1023, 116}),
    },
    // Dialog lines
    SpriteInfo{
        .position = {-39, static_cast<float>(479 - 67 + 39 - DIALOG_LINE_HEIGHT), 639 + 39,
                     static_cast<float>(479 - 67 + 39)},
        .uv = ScaleUv({209, 155, 1023, 186}),
    },
    SpriteInfo{
        .position = {-39, static_cast<float>(479 - 67 + 39 - DIALOG_LINE_HEIGHT), 639 + 39,
                     static_cast<float>(479 - 67 + 39)},
        .uv = ScaleUv({209, 119, 1023, 156}),
    },
    // Divider
    SpriteInfo{
        .position = {35, static_cast<float>(450 - (DIVIDER_HEIGHT / 2)), 605,
                     static_cast<float>(450 + (DIVIDER_HEIGHT + 2))},
        .uv = ScaleUv({209, 94, 602, 116}),
    },
};

constexpr auto SPRITE_COUNT = SPRITE_DATA.size() + (DIALOG_MAX_LINES - 1);

} // namespace

VDX9RENDER *LegacyDialog::RenderService = nullptr;

LegacyDialog::~LegacyDialog() noexcept
{
    core.SetTimeScale(1.f);

    if (interfaceTexture_)
    {
        RenderService->TextureRelease(interfaceTexture_);
    }
}

bool LegacyDialog::Init()
{
    RenderService = static_cast<VDX9RENDER *>(core.GetService("dx9render"));
    Assert(RenderService != nullptr);

    soundService_ = static_cast<VSoundService *>(core.GetService("SoundService"));

    core.SetTimeScale(0.f);

    LoadIni();

    UpdateScreenSize();

    const char *texture = this->AttributesPointer->GetAttribute("texture");
    if (texture == nullptr)
    {
        texture = DEFAULT_INTERFACE_TEXTURE;
    }
    interfaceTexture_ = RenderService->TextureCreate(texture);

    CreateBackBuffers();

    return true;
}

void LegacyDialog::ProcessStage(Stage stage, uint32_t delta)
{
    switch (stage)
    {
    case Stage::realize:
        Realize(delta);
        break;
    }
}

void LegacyDialog::Realize(uint32_t deltaTime)
{
    Unfade();

    if (soundState_ == SOUND_STARTING && !soundName_.empty() && soundService_)
    {
        currentSound_ = soundService_->SoundPlay(soundName_.c_str(), PCM_STEREO, VOLUME_SPEECH);
        if (currentSound_)
        {
            SetAction("dialog_all");
            soundState_ = SOUND_PLAYING;
        }
    }

    UpdateScreenSize();

    ProcessControls();

    if (backNeedsUpdate_)
    {
        UpdateBackBuffers();
    }

    const bool shouldDrawDivider = !formattedLinks_.empty();
    DrawBackground(2, 5 + textureLines_);
    DrawBackground(7 + DIALOG_MAX_LINES, shouldDrawDivider ? 2 : 1);

    DrawHeadModel(deltaTime);

    if (!characterName_.empty())
    {
        RenderService->ExtPrint(nameFont_, COLOR_NORMAL, 0, PR_ALIGN_LEFT, true, fontScale_, 0, 0,
                                static_cast<int32_t>(screenScale_.x * 168), static_cast<int32_t>(screenScale_.y * 28),
                                characterName_.c_str());
    }

    DrawLinks();
    DrawDialogText();

    // Head overlay
    DrawBackground(0, 2);

    if (soundState_ == SOUND_PLAYING && soundService_ && !soundService_->SoundIsPlaying(currentSound_))
    {
        SetAction("dialog_idle");
        soundState_ = SOUND_STOPPED;
    }
}

uint32_t LegacyDialog::AttributeChanged(ATTRIBUTES *attributes)
{
    const std::string_view attributeName = attributes->GetThisName();

    if (storm::iEquals(attributeName, "texture"))
    {
        RenderService->TextureRelease(interfaceTexture_);
        interfaceTexture_ = RenderService->TextureCreate(attributes->GetThisAttr());
    }
    else if (storm::iEquals(attributeName, "headModel"))
    {
        UpdateHeadModel(attributes->GetValue());
    }
    else if (storm::iEquals(attributeName, "mood"))
    {
        const std::string mood = attributes->GetThisAttr();
        mood_ = mood;
    }
    else if (storm::iEquals(attributeName, "greeting"))
    {
        const std::string soundName = attributes->GetThisAttr();
        soundName_ = soundName;
        soundState_ = SOUND_STARTING;
    }
    else
    {
        UpdateLinks();
        UpdateDialogText();
    }

    return Entity::AttributeChanged(attributes);
}

uint64_t LegacyDialog::ProcessMessage(MESSAGE &msg)
{
    switch (msg.Long())
    {
    case 0: {
        // Get character ID
        // persId = msg.EntityID();
        // persMdl = msg.EntityID();
        break;
    }
    case 1: {
        // Get person ID
        const entid_t charId = msg.EntityID();
        const entid_t charModel = msg.EntityID();
        const auto name_attr = core.Entity_GetAttribute(charId, "name");
        const auto last_name_attr = core.Entity_GetAttribute(charId, "lastname");
        const std::string_view name = name_attr ? name_attr : "";
        const std::string_view last_name = last_name_attr ? last_name_attr : "";
        characterName_ = fmt::format("{} {}", name, last_name);
        std::transform(characterName_.begin(), characterName_.end(), characterName_.begin(), ::toupper);
        break;
    }
    }
    return 0;
}

void LegacyDialog::LoadIni()
{
    auto ini = fio->OpenIniFile(DIALOG_INI_FILE_PATH.data());

    mainFont_ = LoadFont("mainfont", *ini, *RenderService);
    nameFont_ = LoadFont("namefont", *ini, *RenderService);
    subFont_ = LoadFont("subfont", *ini, *RenderService);

    ini.reset();
}

void LegacyDialog::UpdateScreenSize()
{
    D3DVIEWPORT9 viewport;
    RenderService->GetViewport(&viewport);
    const auto screenSize = core.GetScreenSize();

    const auto hScale = static_cast<float>(viewport.Width) / static_cast<float>(screenSize.width);
    const auto vScale = static_cast<float>(viewport.Height) / static_cast<float>(screenSize.height);

    if (fabs(screenScale_.x - hScale) > 1e-3f || fabs(screenScale_.y - vScale) > 1e-3f)
    {
        screenScale_.x = hScale;
        screenScale_.y = vScale;

        backNeedsUpdate_ = true;
    }

    const float oldFontScale = fontScale_;
    fontScale_ = static_cast<float>(viewport.Height) / 600.f;
    if (fabs(fontScale_ - oldFontScale) > 1e-3f)
    {
        lineHeight_ = static_cast<int32_t>(static_cast<float>(RenderService->CharHeight(mainFont_)) * fontScale_);
    }
}

void LegacyDialog::CreateBackBuffers()
{
    constexpr auto vertex_count = SPRITE_COUNT * 4;
    constexpr auto index_count = SPRITE_COUNT * 6;

    backVertexBuffer_ =
        RenderService->CreateVertexBuffer(XI_TEX_FVF, vertex_count * sizeof(XI_TEX_VERTEX), D3DUSAGE_WRITEONLY);

    backIndexBuffer_ = RenderService->CreateIndexBuffer(index_count * sizeof(uint16_t));
    FillIndexBuffer(*RenderService, backIndexBuffer_, SPRITE_COUNT);
}

void LegacyDialog::UpdateBackBuffers()
{
    float hScale = screenScale_.x;
    float vScale = screenScale_.y;

    auto createSpriteMesh = [hScale, vScale](const SpriteInfo &sprite) {
        const std::array mesh = {
            XI_TEX_VERTEX{.pos{hScale * sprite.position.left, vScale * sprite.position.top, 1.f},
                          .rhw = 0.5f,
                          .color = COLOR_NORMAL,
                          .u = sprite.uv.left,
                          .v = sprite.uv.top},
            XI_TEX_VERTEX{.pos{hScale * sprite.position.right, vScale * sprite.position.top, 1.f},
                          .rhw = 0.5f,
                          .color = COLOR_NORMAL,
                          .u = sprite.uv.right,
                          .v = sprite.uv.top},
            XI_TEX_VERTEX{.pos{hScale * sprite.position.left, vScale * sprite.position.bottom, 1.f},
                          .rhw = 0.5f,
                          .color = COLOR_NORMAL,
                          .u = sprite.uv.left,
                          .v = sprite.uv.bottom},
            XI_TEX_VERTEX{.pos{hScale * sprite.position.right, vScale * sprite.position.bottom, 1.f},
                          .rhw = 0.5f,
                          .color = COLOR_NORMAL,
                          .u = sprite.uv.right,
                          .v = sprite.uv.bottom},
        };
        return mesh;
    };

    auto *pV = static_cast<XI_TEX_VERTEX *>(RenderService->LockVertexBuffer(backVertexBuffer_));
    size_t vi = 0;
    for (size_t i = 0; i < 7; ++i)
    {
        const auto &vertices = createSpriteMesh(SPRITE_DATA[i]);
        pV[vi++] = vertices[0];
        pV[vi++] = vertices[1];
        pV[vi++] = vertices[2];
        pV[vi++] = vertices[3];
    }

    SpriteInfo sprite_data{};

    for (size_t i = 0; i < DIALOG_MAX_LINES; ++i)
    {
        sprite_data = SPRITE_DATA[7];
        const auto offset = static_cast<float>(DIALOG_LINE_HEIGHT * i);
        sprite_data.position.top -= offset;
        sprite_data.position.bottom -= offset;

        const auto &vertices = createSpriteMesh(sprite_data);
        pV[vi++] = vertices[0];
        pV[vi++] = vertices[1];
        pV[vi++] = vertices[2];
        pV[vi++] = vertices[3];
    }

    // Top of main dialog
    {
        const size_t text_lines = formattedDialogText_.size() + formattedLinks_.size();
        textureLines_ = static_cast<int32_t>(
            std::floor(static_cast<double>((text_lines * lineHeight_) / vScale) / DIALOG_LINE_HEIGHT));

        if (!formattedLinks_.empty())
        {
            textureLines_ += 1;
        }

        sprite_data = SPRITE_DATA[8];
        const auto offset = DIALOG_LINE_HEIGHT * static_cast<float>(textureLines_);
        sprite_data.position.top -= offset;
        sprite_data.position.bottom -= offset;

        const auto &vertices = createSpriteMesh(sprite_data);
        pV[vi++] = vertices[0];
        pV[vi++] = vertices[1];
        pV[vi++] = vertices[2];
        pV[vi++] = vertices[3];
    }

    // Divider
    {
        sprite_data = SPRITE_DATA[9];
        const auto offset = static_cast<float>(formattedLinks_.size()) * static_cast<float>(lineHeight_) / vScale;
        sprite_data.position.top -= offset;
        sprite_data.position.bottom -= offset;

        const auto &vertices = createSpriteMesh(sprite_data);
        pV[vi++] = vertices[0];
        pV[vi++] = vertices[1];
        pV[vi++] = vertices[2];
        pV[vi++] = vertices[3];
    }

    RenderService->UnLockVertexBuffer(backVertexBuffer_);

    backNeedsUpdate_ = false;
}

void LegacyDialog::DrawBackground(size_t start, size_t count)
{
    RenderService->TextureSet(0, interfaceTexture_);
    RenderService->DrawBuffer(backVertexBuffer_, sizeof(XI_TEX_VERTEX), backIndexBuffer_, 0, SPRITE_COUNT * 4,
                              start * 6, count * 2, "texturedialogfon");
}

void LegacyDialog::SetAction(std::string action)
{
    if (!headModel_)
        return;

    std::string preparedAction = action;

    const auto model = dynamic_cast<MODEL *>(core.GetEntityPointer(headModel_));

    if (mood_ != "normal")
    {
        preparedAction += "_" + mood_;
    };

    model->GetAnimation()->CopyPlayerState(0, 1);

    model->GetAnimation()->Player(0).SetAction(preparedAction.c_str());
    model->GetAnimation()->Player(0).Play();

    model->GetAnimation()->Timer(0).ResetTimer();
    model->GetAnimation()->Timer(0).Start(0.2f);
    model->GetAnimation()->Player(0).SetAutoStop(false);
    model->GetAnimation()->Player(1).SetAutoStop(true);
    model->GetAnimation()->Timer(0).SetPlayer(0, false);
    model->GetAnimation()->Timer(0).SetPlayer(1, true);
}

void LegacyDialog::UpdateHeadModel(const std::string &headModelPath)
{
    core.EraseEntity(headModel_);

    const std::string newHeadModelPath = fmt::format("Heads/{}", headModelPath);

    if (headModelPath_ != newHeadModelPath)
    {
        headModelPath_ = newHeadModelPath;
        headModel_ = core.CreateEntity("MODELR");
        auto gs = static_cast<VGEOMETRY *>(core.GetService("geometry"));
        gs->SetTexturePath("characters\\");

        core.Send_Message(headModel_, "ls", MSG_MODEL_LOAD_GEO, headModelPath_.c_str());
        core.Send_Message(headModel_, "ls", MSG_MODEL_LOAD_ANI, headModelPath_.c_str());

        const auto model = dynamic_cast<MODEL *>(core.GetEntityPointer(headModel_));

        static CMatrix mtx;
        mtx.BuildPosition(0.f, 0.025f, 0.f);

        static CMatrix mtx2;
        mtx2.m[0][0] = 1.0f;
        mtx2.m[1][1] = 1.0f;
        mtx2.m[2][2] = 1.0f;

        static CMatrix mtx3;
        mtx3.BuildMatrix(0.1f, PI - 0.1f, 0.0f);

        static CMatrix mtx4;
        mtx4.BuildPosition(0.f, 0.f, 4.f);

        model->mtx = mtx * mtx2 * mtx3 * mtx4;

        SetAction("dialog_idle");

        gs->SetTexturePath("");
    }
}

void LegacyDialog::DrawHeadModel(uint32_t deltaTime)
{
    if (headModel_)
    {
        D3DVIEWPORT9 viewport;
        RenderService->GetViewport(&viewport);

        CMatrix mtx, view, prj;
        uint32_t lightingState, zenableState;
        uint32_t zWriteState{};
        RenderService->GetTransform(D3DTS_VIEW, view);
        RenderService->GetTransform(D3DTS_PROJECTION, prj);
        RenderService->GetRenderState(D3DRS_LIGHTING, &lightingState);
        RenderService->GetRenderState(D3DRS_ZENABLE, &zenableState);
        RenderService->GetRenderState(D3DRS_ZWRITEENABLE, &zWriteState);

        mtx.BuildViewMatrix(CVECTOR(0.0f, 0.0f, 0.0f), CVECTOR(0.0f, 0.0f, 1.0f), CVECTOR(0.0f, 1.0f, 0.0f));
        RenderService->SetTransform(D3DTS_VIEW, (D3DMATRIX *)&mtx);

        mtx.BuildProjectionMatrix(PId2 - 1.49f, screenScale_.x * 116, screenScale_.y * 158, 1.0f, 10.0f);
        RenderService->SetTransform(D3DTS_PROJECTION, (D3DMATRIX *)&mtx);

        D3DVIEWPORT9 headViewport{};
        headViewport.X = static_cast<int32_t>(screenScale_.x * 31);
        headViewport.Y = static_cast<int32_t>(screenScale_.y * 28);
        headViewport.Width = static_cast<int32_t>(screenScale_.x * 115);
        headViewport.Height = static_cast<int32_t>(screenScale_.y * 157);
        headViewport.MinZ = 0.0f;
        headViewport.MaxZ = 1.0f;

        RenderService->SetViewport(&headViewport);
        RenderService->Clear(0, 0, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
        RenderService->SetRenderState(D3DRS_LIGHTING, TRUE);
        RenderService->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        RenderService->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);

        D3DLIGHT9 oldLight{};
        BOOL oldLightEnabled = FALSE;
        RenderService->GetLight(0, &oldLight);
        RenderService->GetLightEnable(0, &oldLightEnabled);

        D3DLIGHT9 headLight{};
        headLight.Type = D3DLIGHT_DIRECTIONAL;
        headLight.Diffuse.r = 1.f;
        headLight.Diffuse.g = 1.f;
        headLight.Diffuse.b = 1.f;
        headLight.Diffuse.a = 1.f;
        headLight.Direction.x = -1.f;
        headLight.Direction.y = -1.f;
        headLight.Direction.z = 2.f;

        RenderService->SetLight(0, &headLight);
        RenderService->LightEnable(0, TRUE);
        const auto model = dynamic_cast<MODEL *>(core.GetEntityPointer(headModel_));
        model->ProcessStage(Entity::Stage::realize, deltaTime);

        RenderService->SetLight(0, &oldLight);
        RenderService->LightEnable(0, oldLightEnabled);
        RenderService->SetTransform(D3DTS_VIEW, (D3DMATRIX *)&view);
        RenderService->SetTransform(D3DTS_PROJECTION, (D3DMATRIX *)&prj);
        RenderService->SetViewport(&viewport);
        RenderService->SetRenderState(D3DRS_LIGHTING, lightingState);
        RenderService->SetRenderState(D3DRS_ZENABLE, zenableState);
        RenderService->SetRenderState(D3DRS_ZWRITEENABLE, zWriteState);
    }
}

void LegacyDialog::UpdateLinks()
{
    const size_t previous_link_lines = formattedLinks_.size();

    links_.clear();
    formattedLinks_.clear();
    ATTRIBUTES *links_attr = AttributesPointer->GetAttributeClass("Links");
    if (links_attr)
    {
        D3DVIEWPORT9 vp;
        RenderService->GetViewport(&vp);

        const auto text_width_limit = static_cast<int32_t>(570.f * (vp.Width / 640.f));

        const size_t number_of_links = links_attr->GetAttributesNum();
        for (size_t i = 0; i < number_of_links; ++i)
        {
            const std::string_view link_text = links_attr->GetAttributeClass(i)->GetValue();
            links_.emplace_back(link_text);

            std::vector<std::string> link_texts;
            DIALOG::AddToStringArrayLimitedByWidth(link_text, subFont_, fontScale_, text_width_limit, link_texts,
                                                   RenderService, nullptr, 0);

            for (const auto &text : link_texts)
            {
                formattedLinks_.emplace_back(LinkEntry{text, static_cast<int32_t>(i)});
            }
        }
    }

    if (previous_link_lines != formattedLinks_.size())
    {
        backNeedsUpdate_ = true;
    }
}

void LegacyDialog::DrawLinks()
{
    if (!formattedLinks_.empty())
    {
        int32_t line_offset = 0;
        int32_t offset = lineHeight_ * static_cast<int32_t>(formattedLinks_.size());
        for (auto &link : formattedLinks_)
        {
            const bool isSelected = link.lineIndex == selectedLink_;
            RenderService->ExtPrint(subFont_, isSelected ? COLOR_NORMAL : COLOR_LINK_UNSELECTED, 0, PR_ALIGN_LEFT, true,
                                    fontScale_, 0, 0, static_cast<int32_t>(screenScale_.x * 35),
                                    static_cast<int32_t>(screenScale_.y * 450 - offset) + line_offset,
                                    link.text.c_str());
            line_offset += lineHeight_;
        }
    }
}

void LegacyDialog::UpdateDialogText()
{
    const size_t previous_lines = formattedDialogText_.size();

    const char *text_attr = AttributesPointer->GetAttribute("Text");
    if (text_attr)
    {
        dialogText_ = text_attr;
    }
    formattedDialogText_.clear();
    if (!dialogText_.empty())
    {
        D3DVIEWPORT9 vp;
        RenderService->GetViewport(&vp);

        const int32_t text_width_limit = static_cast<int32_t>(570.f * (vp.Width / 640.f));

        DIALOG::AddToStringArrayLimitedByWidth(dialogText_, mainFont_, fontScale_, text_width_limit,
                                               formattedDialogText_, RenderService, nullptr, 0);
    }

    if (previous_lines != formattedDialogText_.size())
    {
        backNeedsUpdate_ = true;
    }
}

void LegacyDialog::DrawDialogText()
{
    if (!dialogText_.empty())
    {
        int32_t line_offset = 0;

        const auto offset =
            static_cast<int32_t>(screenScale_.y * static_cast<float>(445 - textureLines_ * DIALOG_LINE_HEIGHT));

        for (const std::string &text : formattedDialogText_)
        {
            RenderService->ExtPrint(mainFont_, COLOR_NORMAL, 0, PR_ALIGN_LEFT, true, fontScale_, 0, 0,
                                    static_cast<int32_t>(screenScale_.x * 35), offset + line_offset, text.c_str());
            line_offset += lineHeight_;
        }
    }
}

void LegacyDialog::ProcessControls()
{
    CONTROL_STATE cs;
    bool bDoUp = false;
    bool bDoDown = false;
    bool bDoAction = false;

    core.Controls->GetControlState("DlgUp", cs);
    if (cs.state == CST_ACTIVATED)
    {
        bDoUp = true;
    }

    core.Controls->GetControlState("DlgUp2", cs);
    if (cs.state == CST_ACTIVATED)
    {
        bDoUp = true;
    }

    core.Controls->GetControlState("DlgUp3", cs);
    if (cs.state == CST_ACTIVATED)
    {
        bDoUp = true;
    }

    if (bDoUp && selectedLink_ > 0)
    {
        PlayTick();
        --selectedLink_;
    }

    core.Controls->GetControlState("DlgDown", cs);
    if (cs.state == CST_ACTIVATED)
    {
        bDoDown = true;
    }

    core.Controls->GetControlState("DlgDown2", cs);
    if (cs.state == CST_ACTIVATED)
    {
        bDoDown = true;
    }

    core.Controls->GetControlState("DlgDown3", cs);
    if (cs.state == CST_ACTIVATED)
    {
        bDoDown = true;
    }

    if (bDoDown && selectedLink_ < links_.size() - 1)
    {
        PlayTick();
        ++selectedLink_;
    }

    core.Controls->GetControlState("DlgAction", cs);
    if (cs.state == CST_ACTIVATED)
    {
        bDoAction = true;
    }

    core.Controls->GetControlState("DlgAction1", cs);
    if (cs.state == CST_ACTIVATED)
    {
        bDoAction = true;
    }

    core.Controls->GetControlState("DlgAction2", cs);
    if (cs.state == CST_ACTIVATED)
    {
        bDoAction = true;
    }

    if (bDoAction)
    {
        PlayTick();
        ATTRIBUTES *links_attr = AttributesPointer->GetAttributeClass("Links");
        if (links_attr)
        {
            ATTRIBUTES *selected_attr = links_attr->GetAttributeClass(selectedLink_);
            if (selected_attr)
            {
                const char *go = selected_attr->GetAttribute("go");
                AttributesPointer->SetAttribute("CurrentNode", go);
                selectedLink_ = 0;
                core.Event("DialogEvent");
            }
        }
    }
}

void LegacyDialog::PlayTick()
{
    if (soundService_)
    {
        soundService_->SoundPlay(TICK_SOUND, PCM_STEREO, VOLUME_FX);
    }
}

void LegacyDialog::Unfade()
{
    // delayed exit from pause
    if (fadeTime_ <= UNFADE_TIME)
    {
        fadeTime_ += static_cast<int>(core.GetRDeltaTime());
        float timeK = static_cast<float>(fadeTime_) / UNFADE_TIME;
        if (timeK > 1.f)
            timeK = 1.f;
        core.SetTimeScale(timeK);
    }
}
