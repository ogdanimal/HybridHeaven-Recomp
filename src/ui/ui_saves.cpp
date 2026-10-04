#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "ui_saves.h"

#include "recomp_ui.h"
#include "hh_support.h"
#include "librecomp/game.hpp"
#include "ultramodern/ultramodern.hpp"

// The "Saves" settings category. See ui_saves.h for why it exists.
//
// TWO RULES SHAPE EVERYTHING BELOW, both about not destroying save data.
//
// 1. Importing is refused once the game has started. The running game holds the
//    save in emulated flash and writes that back out on its own schedule, so a
//    file dropped underneath it is either ignored or overwritten within
//    minutes -- and which of the two you get depends on timing, which is the
//    worst possible property for something handling save data. Before the game
//    starts there is no such copy: init_saving() reads the file, so an import
//    is simply what the game will find. Refusing outright is the only version
//    of this that is always right.
//
// 2. The save being replaced is copied aside first, and the import is staged
//    and renamed rather than written in place. Import is the one operation here
//    whose entire purpose is to overwrite the file the player cares most about.

extern std::vector<recomp::GameEntry> supported_games;

namespace {

struct SavesModelContext {
    // Shown as-is. Built by refresh_state() so the strings the document binds
    // never have to be assembled in the template.
    std::string save_path;
    std::string save_detail;
    std::string message;

    bool importable = false;      // no game running yet -> import is allowed
    bool has_save = false;
    bool game_started = false;    // cached, so tick_saves does no work per frame

    int focused_option = -1;
    Rml::DataModelHandle model_handle;
};

SavesModelContext saves_ctx;

const recomp::GameEntry& game_entry() {
    return supported_games.at(0);
}

std::filesystem::path save_file_path() {
    // Deliberately NOT ultramodern::get_save_file_path(): that reports the live
    // path and is empty until init_saving() runs, which by rule 1 above is
    // exactly when this feature is not allowed to act.
    return ultramodern::get_save_file_path_for(u8"", game_entry().game_id);
}

// Hybrid Heaven's save layout, from the game's own load and save code in
// .file_7. The runtime maps the Controller Pak's one game file straight onto
// the save .bin at the same offsets (librecomp/src/pak.cpp,
// osPfsReadWriteFile_recomp), so the .bin begins with the game's 0x3500-byte
// pak file: a 0x100-byte directory followed by four 0xD00-byte slots. The rest
// of the .bin is unused. See docs/autosave.md, "The pak layout".
//
// The directory is checked exactly as the game's loader func_80141A74_521184
// checks it, which rejects the whole file (error 0xE) unless BOTH hold:
//   - it begins with the 13 bytes "HYBRID HEAVEN" (D_801814A0_560BB0, which
//     the directory writer func_80141BD0_5212E0 stamps in); and
//   - the low byte of the sum of bytes 0..0xFE equals byte 0xFF.
// Requiring those is therefore exactly as strict as the game, no stricter.
//
// Each slot carries the same kind of checksum -- the low byte of the sum of
// its bytes 0..0xCFE, stored at +0xCFF -- written by func_80141F28_521638 and
// checked by func_80141D08_521418 and func_8014217C_52188C. Note that an
// all-zero slot passes it (sum 0, stored 0), and an unused slot IS all zeroes,
// so a checksum match alone does not mean a slot holds a game.
constexpr size_t save_dir_size = 0x100;
constexpr size_t save_slot_size = 0xD00;
constexpr size_t save_slot_count = 4;
constexpr size_t save_pak_size = save_dir_size + save_slot_count * save_slot_size; // 0x3500
constexpr char save_magic[] = "HYBRID HEAVEN";
constexpr size_t save_magic_len = sizeof(save_magic) - 1; // 13, as the loader compares

// The game's checksum: the low byte of a running sum over [data, data + len).
// (The game accumulates into 16 bits, which cannot change the low byte.)
uint8_t save_sum8(const uint8_t* data, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += data[i];
    }
    return static_cast<uint8_t>(sum);
}

// True if the directory passes the game's own loader checks AND at least one
// slot holds a game: non-zero, with its stored checksum matching its contents.
//
// ANY slot rather than ALL, because unused slots are zeroes. And "non-zero" as
// well as "checksum matches" because this checksum, unlike a CRC, is satisfied
// by an empty slot -- without it, a save that has never been written to would
// pass, and importing it would replace a real save with nothing to load.
//
// The point of the check is that size alone does not tell a save from any
// other file of the same length, including a save from a different emulator,
// which would otherwise import "successfully" and then not appear in the game.
bool contains_save_data(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < save_pak_size) {
        return false;
    }
    if (std::memcmp(bytes.data(), save_magic, save_magic_len) != 0) {
        return false;
    }
    if (save_sum8(bytes.data(), save_dir_size - 1) != bytes[save_dir_size - 1]) {
        return false;
    }
    for (size_t slot = 0; slot < save_slot_count; slot++) {
        const uint8_t* block = bytes.data() + save_dir_size + slot * save_slot_size;
        const bool empty = std::all_of(block, block + save_slot_size, [](uint8_t b) { return b == 0; });
        if (!empty && save_sum8(block, save_slot_size - 1) == block[save_slot_size - 1]) {
            return true;
        }
    }
    return false;
}

std::string human_size(uintmax_t bytes) {
    if (bytes < 1024) {
        return std::to_string(bytes) + " bytes";
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.0f KB", static_cast<double>(bytes) / 1024.0);
    return buf;
}

void set_message(std::string text) {
    saves_ctx.message = std::move(text);
    if (saves_ctx.model_handle) {
        saves_ctx.model_handle.DirtyVariable("saves_message");
    }
}

void refresh_state() {
    const std::filesystem::path path = save_file_path();
    saves_ctx.save_path = path.string();

    std::error_code ec;
    saves_ctx.has_save = std::filesystem::is_regular_file(path, ec);
    if (saves_ctx.has_save) {
        const uintmax_t size = std::filesystem::file_size(path, ec);
        saves_ctx.save_detail = ec ? std::string{ "present" } : human_size(size);
    }
    else if (std::filesystem::exists(path, ec)) {
        // Something is there but it is not a regular file. Worth saying, because
        // it is the shape of problem that otherwise only shows up as saving
        // mysteriously failing later.
        saves_ctx.save_detail = "something that is not a save file is at this path";
    }
    else {
        saves_ctx.save_detail = "no save file yet";
    }

    saves_ctx.game_started = ultramodern::is_game_started();
    saves_ctx.importable = !saves_ctx.game_started;

    if (saves_ctx.model_handle) {
        saves_ctx.model_handle.DirtyAllVariables();
    }
}

// Copies `from` to `to` via a staged temp file in the destination's directory,
// so `to` is never observably half-written. Returns an empty string on success
// or a reason on failure.
std::string staged_copy(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::filesystem::path temp = to;
    temp += ".importtemp";

    std::error_code ec;
    std::filesystem::copy_file(from, temp, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        return ec.message();
    }

    std::filesystem::rename(temp, to, ec);
    if (ec) {
        std::error_code cleanup_ec;
        std::filesystem::remove(temp, cleanup_ec);
        return ec.message();
    }
    return {};
}

struct ImportOutcome {
    bool ok = false;
    std::string message;
};

ImportOutcome import_save(const std::filesystem::path& source) {
    if (ultramodern::is_game_started()) {
        // Belt and braces: the button is disabled in this state, but the file
        // dialog is asynchronous, so the game could in principle have been
        // started between the picker opening and the file coming back.
        return { false, "The game started while the file was being chosen. Importing was cancelled." };
    }

    std::error_code ec;
    if (!std::filesystem::is_regular_file(source, ec)) {
        return { false, "That is not a file this app can read." };
    }

    const uintmax_t size = std::filesystem::file_size(source, ec);
    if (ec) {
        return { false, "Could not read that file: " + ec.message() };
    }

    // Size is the only check worth making. The save's own contents are the
    // game's business -- it validates its slots itself and reports an empty file
    // as an empty save -- but a file of the wrong size is definitely not one of
    // ours, and letting it through would replace a real save with something the
    // game cannot use.
    const size_t expected = recomp::get_save_size(game_entry().save_type);
    if (size != expected) {
        return { false, "That file is " + human_size(size) + ". A save file for this game is "
                        + human_size(expected) + ", so this is not one." };
    }

    // Read the pak file only: it is the first 0x3500 bytes, and nothing after it
    // is used.
    std::vector<uint8_t> head(save_pak_size);
    {
        std::ifstream in(source, std::ios::binary);
        if (!in.good()) {
            return { false, "Could not open that file." };
        }
        in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
        if (in.gcount() != static_cast<std::streamsize>(head.size())) {
            return { false, "Could not read that file." };
        }
    }
    if (!contains_save_data(head)) {
        return { false, "That file is the right size but does not hold Hybrid Heaven save data. "
                        "A save from a different emulator can be the same size without being "
                        "the same format." };
    }

    const std::filesystem::path destination = save_file_path();
    const std::filesystem::path folder = destination.parent_path();

    // The specific failure that brought this feature about: a plain file sitting
    // where the saves folder should be, which is what copying a save in by hand
    // produces when the folder does not exist yet. Nothing can be created inside
    // it, and every save fails from then on.
    if (std::filesystem::exists(folder, ec) && !std::filesystem::is_directory(folder, ec)) {
        return { false, "A file is in the way of the save folder \"" + folder.string()
                        + "\". Delete that file and try again -- the folder will be recreated." };
    }

    std::filesystem::create_directories(folder, ec);
    if (ec) {
        return { false, "Could not create the save folder: " + ec.message() };
    }

    // Keep whatever is being replaced. Its own suffix, so this neither disturbs
    // the `.bak` the save rotation owns nor the `.manual.bak` rollback point.
    const bool replacing = std::filesystem::is_regular_file(destination, ec);
    if (replacing) {
        std::filesystem::path backup = destination;
        backup += ".pre-import.bak";
        const std::string failure = staged_copy(destination, backup);
        if (!failure.empty()) {
            return { false, "Could not back up the save that is already there, so nothing was "
                            "changed: " + failure };
        }
    }

    const std::string failure = staged_copy(source, destination);
    if (!failure.empty()) {
        return { false, "Could not copy the save file into place: " + failure };
    }

    // Only claim a backup when one was made: with no save here before, there was
    // nothing to keep, and pointing at a file that does not exist would send
    // someone looking for it.
    if (!replacing) {
        return { true, "Imported. Start the game and load your file as usual." };
    }
    return { true, "Imported. Start the game and load your file as usual. The save that was here "
                   "has been kept as " + destination.filename().string() + ".pre-import.bak" };
}

void on_saves_import() {
    if (!saves_ctx.importable) {
        return;
    }

    // The warning comes BEFORE the picker, not after it. Partly so the choice to
    // overwrite is made knowingly rather than as an afterthought, and partly
    // because opening a prompt from a file-dialog callback means opening it from
    // whatever thread delivered that callback -- which is a road this UI has
    // already been down once, with the prompt context wedging for the session.

    // Say what will actually happen to the file that is there now. There may be
    // none -- a fresh install -- and promising a .pre-import.bak of nothing sends
    // someone looking for a file that will never exist. Re-read first: the tab's
    // cached answer only refreshes when the game starts.
    refresh_state();
    const char* prompt_text = saves_ctx.has_save
        ? "This replaces the save this app is using. The current one is kept alongside it as a "
          ".pre-import.bak file, and can be put back by hand."
        : "There is no save here yet, so nothing will be replaced.";
    recompui::open_choice_prompt(
        "Import a save file?",
        prompt_text,
        "Choose a file",
        "Cancel",
        []() {
            set_message("Choose a save file (.bin).");
            hybridheaven::open_file_dialog([](bool success, const std::filesystem::path& path) {
                if (!success) {
                    // Cancelled. Leaving the "choose a file" line up would imply
                    // the picker is still waiting for them.
                    set_message({});
                    return;
                }
                const ImportOutcome outcome = import_save(path);
                refresh_state();
                set_message(outcome.message);
            });
        },
        []() {},
        recompui::ButtonVariant::Success,
        recompui::ButtonVariant::Tertiary,
        /*focus_on_cancel=*/true,
        "saves_import_button"
    );
}

} // namespace

void recompui::make_saves_bindings(Rml::Context* context) {
    Rml::DataModelConstructor constructor = context->CreateDataModel("saves_model");
    if (!constructor) {
        throw std::runtime_error("Failed to make RmlUi data model for the saves menu");
    }

    constructor.Bind("saves_path", &saves_ctx.save_path);
    constructor.Bind("saves_detail", &saves_ctx.save_detail);
    constructor.Bind("saves_message", &saves_ctx.message);
    constructor.Bind("saves_importable", &saves_ctx.importable);
    constructor.Bind("saves_has_save", &saves_ctx.has_save);

    // Same contract as every other config tab's description panel: the focused
    // row's index picks the paragraph.
    constructor.Bind("cur_config_index", &saves_ctx.focused_option);
    constructor.BindEventCallback("set_cur_config_index",
        [](Rml::DataModelHandle model_handle, Rml::Event& event, const Rml::VariantList& inputs) {
            int option_index = inputs.at(0).Get<size_t>();
            // mouseout bubbles, so only the element owning the row clears it.
            if (option_index == -1 && event.GetType() == "mouseout"
                    && event.GetCurrentElement() != event.GetTargetElement()) {
                return;
            }
            saves_ctx.focused_option = option_index;
            model_handle.DirtyVariable("cur_config_index");
        });

    saves_ctx.model_handle = constructor.GetModelHandle();

    refresh_state();
}

void recompui::register_saves_events(recompui::UiEventListenerInstancer& listener) {
    recompui::register_event(listener, "saves_import",
        [](const std::string& /*param*/, Rml::Event& /*event*/) {
            on_saves_import();
        });
}

void recompui::tick_saves() {
    // Only the game-started answer is checked per frame; it is an atomic load.
    // Everything else costs file I/O and is refreshed only when that flips,
    // which happens at most once per run.
    const bool started = ultramodern::is_game_started();
    if (started == saves_ctx.game_started) {
        return;
    }
    refresh_state();
}
