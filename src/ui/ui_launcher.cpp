#include "recomp_ui.h"
#include "hh_config.h"
#include "hh_support.h"
#include "librecomp/game.hpp"
#include "ultramodern/ultramodern.hpp"
#include "RmlUi/Core.h"
#if !defined(__ANDROID__)
#include "nfd.h"
#endif
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

static std::string version_string;

// The launcher's cover art, and the name assets/launcher.rml refers to it by.
//
// The UI renderer does not load textures from disk at all: LoadTexture looks the
// source string up in a map of images registered as BYTES, and returns a 1x1
// transparent texture when it misses -- silently, which is why this function
// reports its own failures. The `?/` prefix keeps RmlUi from resolving the name
// as a path relative to the document, the same trick the mod menu uses for its
// thumbnails (generate_thumbnail_src_for_mod).
static constexpr const char* launcher_background_src = "?/launcher/background";

static void register_launcher_background() {
    static bool attempted = false;
    if (attempted) {
        return;
    }
    attempted = true;

    const std::filesystem::path path = hybridheaven::get_asset_path("launcher_background.png");
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        fprintf(stderr, "[ui] launcher background not found at %s -- the launcher will "
                        "draw its flat background instead\n", path.string().c_str());
        return;
    }

    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> bytes(static_cast<size_t>(size > 0 ? size : 0));
    if (size <= 0 || !file.read(bytes.data(), size)) {
        fprintf(stderr, "[ui] launcher background at %s could not be read\n",
            path.string().c_str());
        return;
    }

    recompui::queue_image_from_bytes_file(launcher_background_src, bytes);

    // One line, always. The renderer substitutes a 1x1 transparent texture for an
    // image it cannot find and reports success, so "the launcher looks plain" has
    // two causes that look identical from the outside -- the art never registered,
    // or it registered and the styling puts it somewhere invisible. This line
    // separates them without a rebuild.
    fprintf(stderr, "[ui] launcher background registered as \"%s\" (%zu bytes from %s)\n",
        launcher_background_src, bytes.size(), path.string().c_str());
}

Rml::DataModelHandle model_handle;
bool rom_valid = false;

extern std::vector<recomp::GameEntry> supported_games;

void select_rom() {
#if !defined(__ANDROID__)
    nfdnchar_t* native_path = nullptr;
    (void)native_path;
#endif
    hybridheaven::open_file_dialog([](bool success, const std::filesystem::path& path) {
        if (success) {
            recomp::RomValidationError rom_error = recomp::select_rom(path, supported_games[0].game_id);
            switch (rom_error) {
                case recomp::RomValidationError::Good:
                    rom_valid = true;
                    model_handle.DirtyVariable("rom_valid");
                    break;
                case recomp::RomValidationError::FailedToOpen:
                    recompui::message_box("Failed to open ROM file.");
                    break;
                case recomp::RomValidationError::NotARom:
                    recompui::message_box("This is not a valid ROM file.");
                    break;
                // librecomp checks the hash first and the header name second, so
                // a file that hashes wrong but says HYBRIDHEAVEN at 0x20 comes
                // back as IncorrectVersion -- another region, a patched ROM, or
                // the pre-decompressed image this port asked for before it took
                // the retail dump. IncorrectRom means some other game entirely.
                // Byteswapped (.v64/.n64) dumps are normalised before hashing.
                case recomp::RomValidationError::IncorrectRom:
                    recompui::message_box(
                            "This is not a Hybrid Heaven ROM.\n\n"
                            "This port needs a dump of the NTSC-U (USA) cartridge -- "
                            "sha1 16dbc21620b52deab5c5abf8a309ac60adfbee85.");
                    break;
                case recomp::RomValidationError::NotYet:
                    recompui::message_box("This game isn't supported yet.");
                    break;
                case recomp::RomValidationError::IncorrectVersion:
                    recompui::message_box(
                            "This is Hybrid Heaven, but not the version this port expects.\n\n"
                            "It needs an unmodified dump of the NTSC-U (USA) cartridge -- "
                            "sha1 16dbc21620b52deab5c5abf8a309ac60adfbee85. Other regions, "
                            "patched ROMs and pre-decompressed images will not match.");
                    break;
                case recomp::RomValidationError::OtherError:
                    recompui::message_box("An unknown error has occurred.");
                    break;
            }
        }
    });
}

recompui::ContextId launcher_context;

recompui::ContextId recompui::get_launcher_context_id() {
	return launcher_context;
}

class LauncherMenu : public recompui::MenuController {
public:
    LauncherMenu() {
        rom_valid = recomp::is_rom_valid(supported_games[0].game_id);
    }
    ~LauncherMenu() override {

    }
    void load_document() override {
		// Before the document, not after: the <img> in it resolves its source the
		// first time it is laid out, and a name that is not registered by then
		// resolves to the 1x1 transparent placeholder and stays there.
		register_launcher_background();
		launcher_context = recompui::create_context(hybridheaven::get_asset_path("launcher.rml"));
    }
    void register_events(recompui::UiEventListenerInstancer& listener) override {
        recompui::register_event(listener, "select_rom",
            [](const std::string& param, Rml::Event& event) {
                select_rom();
            }
        );
        recompui::register_event(listener, "rom_selected",
            [](const std::string& param, Rml::Event& event) {
                rom_valid = true;
                model_handle.DirtyVariable("rom_valid");
            }
        );
        recompui::register_event(listener, "start_game",
            [](const std::string& param, Rml::Event& event) {
                recomp::start_game(supported_games[0].game_id);
                recompui::hide_all_contexts();
            }
        );
        recompui::register_event(listener, "open_controls",
            [](const std::string& param, Rml::Event& event) {
                recompui::set_config_tab(recompui::ConfigTab::Controls);
                recompui::hide_all_contexts();
                recompui::show_context(recompui::get_config_context_id(), "");
            }
        );
        recompui::register_event(listener, "open_settings",
            [](const std::string& param, Rml::Event& event) {
                recompui::set_config_tab(recompui::ConfigTab::General);
                recompui::hide_all_contexts();
                recompui::show_context(recompui::get_config_context_id(), "");
            }
        );
        recompui::register_event(listener, "open_mods",
            [](const std::string &param, Rml::Event &event) {
                recompui::set_config_tab(recompui::ConfigTab::Mods);
                recompui::hide_all_contexts();
                recompui::show_context(recompui::get_config_context_id(), "");
            }
        );
        recompui::register_event(listener, "exit_game",
            [](const std::string& param, Rml::Event& event) {
                ultramodern::quit();
            }
        );
    }
    void make_bindings(Rml::Context* context) override {
        Rml::DataModelConstructor constructor = context->CreateDataModel("launcher_model");

        constructor.Bind("rom_valid", &rom_valid);

        version_string = recomp::get_project_version().to_string();
        constructor.Bind("version_number", &version_string);

        model_handle = constructor.GetModelHandle();
    }
};

std::unique_ptr<recompui::MenuController> recompui::create_launcher_menu() {
    return std::make_unique<LauncherMenu>();
}
