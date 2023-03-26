/***************************************************************************
 *   Copyright (C) 2020 PCSX-Redux authors                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

#include "core/binloader.h"

#include <stdint.h>

#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include "core/psxemulator.h"
#include "core/r3000a.h"
#include "elfio/elfio.hpp"
#include "fmt/format.h"
#include "support/file.h"
#include "support/stream-file.h"
#include "support/strings-helpers.h"
#include "support/zfile.h"

namespace PCSX {

namespace {

bool loadCPE(IO<File> file, IO<File> dest) {
    uint32_t magic = file->read<uint32_t>();
    if (magic != 0x1455043) return false;
    auto& regs = g_emulator->m_cpu->m_regs;
    file->read<uint16_t>();

    uint8_t opcode;

    while ((opcode = file->byte())) {
        uint16_t reg;
        uint32_t value;
        bool setRegister = false;

        switch (opcode) {
            case 1: {  // load
                uint32_t addr = file->read<uint32_t>();
                uint32_t size = file->read<uint32_t>();
                dest->writeAt(file->read(size), addr);
            } break;
            case 2: {
                file->read<uint32_t>();
            } break;
            case 3: {
                reg = file->read<uint16_t>();
                value = file->read<uint32_t>();
                setRegister = true;
            } break;
            case 4: {
                reg = file->read<uint16_t>();
                value = file->read<uint16_t>();
                setRegister = true;
            } break;
            case 5: {
                reg = file->read<uint16_t>();
                value = file->read<uint8_t>();
                setRegister = true;
            } break;
            case 6: {
                reg = file->read<uint16_t>();
                value = file->read<uint16_t>();
                uint32_t remainder = file->read<uint8_t>();
                value |= remainder << 16;
                setRegister = true;
            } break;
            case 7: {
                file->read<uint32_t>();
            } break;
            case 8: {
                file->byte();
            } break;
        }

        if (setRegister) {
            switch (reg) {
                case 0x90: {
                    regs.pc = value;
                } break;
            }
        }
    }

    return true;
}

bool loadPSEXE(IO<File> file, IO<File> dest) {
    uint64_t magic = file->read<uint64_t>();
    if (magic != 0x45584520582d5350) return false;

    auto& regs = g_emulator->m_cpu->m_regs;

    file->read<uint32_t>();
    file->read<uint32_t>();

    regs.pc = file->read<uint32_t>();
    file->read<uint32_t>();
    uint32_t addr = file->read<uint32_t>();
    uint32_t size = file->read<uint32_t>();
    file->read<uint32_t>();
    file->read<uint32_t>();
    file->read<uint32_t>();
    file->read<uint32_t>();
    const auto sp = file->read<uint32_t>();
    if (sp != 0) regs.GPR.n.sp = sp;
    file->rSeek(0x71, SEEK_SET);
    uint8_t region = file->byte();
    file->rSeek(2048, SEEK_SET);
    dest->writeAt(file->read(size), addr);
    if (g_emulator->settings.get<Emulator::SettingAutoVideo>()) {  // autodetect system (pal or ntsc)
        switch (region) {
            case 'A':
            case 'J':
                g_emulator->settings.get<Emulator::SettingVideo>() = Emulator::PSX_TYPE_NTSC;
                break;
            case 'E':
                g_emulator->settings.get<Emulator::SettingVideo>() = Emulator::PSX_TYPE_PAL;
                break;
        }
    }
    return true;
}

bool loadPSF(IO<File> file, IO<File> dest, bool seenRefresh = false, unsigned depth = 0) {
    if (depth >= 10) return false;
    uint32_t magic = file->read<uint32_t>();
    if (magic != 0x1465350) return false;
    uint32_t R = file->read<uint32_t>();
    uint32_t N = file->read<uint32_t>();
    uint32_t C = file->read<uint32_t>();
    file->rSeek(R, SEEK_CUR);
    IO<File> zpsexe(new SubFile(file, file->rTell(), N));
    file->rSeek(N, SEEK_CUR);
    char tagtag[6];
    file->read(tagtag, 5);
    tagtag[5] = 0;
    std::string tagsStorage;

    std::map<std::string_view, std::string_view> pairs;

    if (strcmp(tagtag, "[TAG]") == 0) {
        size_t tagsSize = file->size() - file->rTell();
        tagsStorage = file->readString(tagsSize);
        std::string_view tags = tagsStorage;

        auto lines = StringsHelpers::split(tags, "\n\r");

        for (auto& line : lines) {
            auto e = line.find('=', 0);
            if (e == std::string::npos) continue;
            pairs[line.substr(0, e)] = line.substr(e + 1);
        }
    }

    if (!seenRefresh && pairs.find("refresh") != pairs.end()) {
        const auto& refresh = pairs["refresh"];
        if (refresh == "50") {
            g_emulator->settings.get<Emulator::SettingVideo>() = Emulator::PSX_TYPE_PAL;
        } else if (refresh == "60") {
            g_emulator->settings.get<Emulator::SettingVideo>() = Emulator::PSX_TYPE_NTSC;
        }
        seenRefresh = true;
    }

    if (pairs.find("_lib") != pairs.end()) {
        std::filesystem::path subFilePath(file->filename());
        IO<File> subFile(new PosixFile(subFilePath.parent_path() / pairs["_lib"]));
        if (!subFile->failed()) loadPSF(subFile, dest, seenRefresh, depth++);
    }

    IO<File> psexe(new ZReader(zpsexe));
    loadPSEXE(psexe, dest);

    unsigned libNum = 2;

    while (true) {
        std::string libName = fmt::format("_lib{}", libNum++);
        if (pairs.find(libName) == pairs.end()) break;
        std::filesystem::path subFilePath(file->filename());
        IO<File> subFile(new PosixFile(subFilePath.parent_path() / pairs[libName]));
        if (!subFile->failed()) loadPSF(subFile, dest, seenRefresh, depth++);
    }

    return true;
}

bool loadELF(IO<File> file, IO<File> dest) {
    using namespace ELFIO;
    elfio reader;
    FileIStream stream(file);

    if (!reader.load(stream)) return false;
    if (reader.get_class() != ELFCLASS32) return false;

    auto& regs = g_emulator->m_cpu->m_regs;
    regs.pc = reader.get_entry();

    Elf_Half sec_num = reader.sections.size();
    for (int i = 0; i < sec_num; i++) {
        const section* psec = reader.sections[i];
        auto name = psec->get_name();

        if (StringsHelpers::endsWith(name, "_Header")) continue;

        auto type = psec->get_type();
        if (type != SHT_PROGBITS) continue;

        auto size = psec->get_size();
        auto data = psec->get_data();
        auto addr = psec->get_address();
        dest->writeAt(data, size, addr);
    }

    return true;
}

}  // namespace

}  // namespace PCSX

bool PCSX::BinaryLoader::load(const std::filesystem::path& filename) {
    IO<File> memFile = g_emulator->m_mem->getMemoryAsFile();

    {
        IO<File> ny(new PosixFile(filename.parent_path() / "libps.exe"));
        if (!ny->failed()) loadPSEXE(ny, memFile);
    }

    IO<File> file(new PosixFile(filename));

    if (file->failed()) return false;
    if (loadCPE(file, memFile)) return true;
    file->rSeek(0, SEEK_SET);
    if (loadPSEXE(file, memFile)) return true;
    file->rSeek(0, SEEK_SET);
    if (loadPSF(file, memFile)) return true;
    file->rSeek(0, SEEK_SET);
    if (loadELF(file, memFile)) return true;
    return false;
}
