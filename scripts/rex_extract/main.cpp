// rex_extract: dump an Xbox 360 disc image (XDVDFS) or STFS package (CON/LIVE/PIRS) to a host
// directory, or a retail XEX2 to its flat PE image, using ReXGlue's own filesystem devices, XEX
// structures and LZX decoder. Read-only on the input.
//
//   rex_extract disc  <image.iso> <out_dir>
//   rex_extract stfs  <package>   <out_dir>
//   rex_extract image <module.xex> <out.image.bin>   # maps at the XEX base address

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <rex/filesystem.h>
#include <rex/filesystem/devices/disc_image_device.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/system/lzx.h>
#include <rex/system/util/xex2_info.h>
#include <rex/system/xex_module.h>

// Exported by librexruntime (the SDK's aes128 library) without a public header.
extern "C" {
void aes_key_schedule_128(const uint8_t* key, uint8_t* roundkeys);
void aes_decrypt_128(const uint8_t* roundkeys, const uint8_t* ciphertext, uint8_t* plaintext);
}

namespace fs = std::filesystem;
using rex::X_STATUS;
using rex::filesystem::Entry;

namespace {

constexpr uint8_t kXex2RetailKey[16] = {0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
                                        0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91};
constexpr uint8_t kXex2DevkitKey[16] = {};

uint32_t Be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

std::vector<uint8_t> AesCbcDecrypt(std::span<const uint8_t> in, const uint8_t key[16]) {
  uint8_t roundkeys[176];
  aes_key_schedule_128(key, roundkeys);
  std::vector<uint8_t> out(in.size() / 16 * 16);
  uint8_t iv[16] = {};
  for (size_t i = 0; i < out.size(); i += 16) {
    aes_decrypt_128(roundkeys, in.data() + i, out.data() + i);
    for (int j = 0; j < 16; ++j) out[i + j] ^= iv[j];
    std::memcpy(iv, in.data() + i, 16);
  }
  return out;
}

// Mirrors XexModule::ReadImage{Uncompressed,BasicCompressed,Compressed}.
int ExtractImage(const fs::path& src, const fs::path& dst) {
  std::ifstream in(src, std::ios::binary);
  std::vector<uint8_t> xex((std::istreambuf_iterator<char>(in)), {});
  if (xex.size() < 24 || std::memcmp(xex.data(), "XEX2", 4) != 0) {
    std::fprintf(stderr, "%s: not an XEX2 file\n", src.c_str());
    return 1;
  }
  auto* header = reinterpret_cast<const rex::xex2_header*>(xex.data());
  const uint32_t pe_offset = Be32(xex.data() + 8);
  const uint8_t* security = xex.data() + Be32(xex.data() + 16);
  const uint32_t image_size = Be32(security + 4);
  uint32_t base = 0;
  rex::xex2_opt_file_format_info* ffi = nullptr;
  void* base_ptr = nullptr;
  // Keys ending in 0x01 hold their value inline; GetOptHeader returns a pointer to it.
  if (rex::runtime::XexModule::GetOptHeader(header, rex::XEX_HEADER_IMAGE_BASE_ADDRESS, &base_ptr)) {
    base = Be32(static_cast<const uint8_t*>(base_ptr));
  }
  if (!rex::runtime::XexModule::GetOptHeader(header, rex::XEX_HEADER_FILE_FORMAT_INFO, &ffi)) {
    std::fprintf(stderr, "%s: no file format info\n", src.c_str());
    return 1;
  }
  const uint16_t encryption = ffi->encryption_type;
  const uint16_t compression = ffi->compression_type;
  std::span<const uint8_t> payload(xex.data() + pe_offset, xex.size() - pe_offset);

  for (const uint8_t* key : {kXex2RetailKey, kXex2DevkitKey}) {
    std::vector<uint8_t> data(payload.begin(), payload.end());
    if (encryption == 1) {
      uint8_t roundkeys[176], session_key[16];
      aes_key_schedule_128(key, roundkeys);
      aes_decrypt_128(roundkeys, security + 0x150, session_key);
      data = AesCbcDecrypt(payload, session_key);
    }

    std::vector<uint8_t> image(image_size);
    if (compression == 0) {
      std::memcpy(image.data(), data.data(), std::min<size_t>(data.size(), image.size()));
    } else if (compression == 1) {
      const uint32_t count = (ffi->info_size - 8) / 8;
      size_t src_pos = 0, dst_pos = 0;
      for (uint32_t i = 0; i < count; ++i) {
        const auto& block = ffi->compression_info.basic.blocks[i];
        const uint32_t data_size = block.data_size, zero_size = block.zero_size;
        if (src_pos + data_size > data.size() || dst_pos + data_size + zero_size > image.size()) break;
        std::memcpy(image.data() + dst_pos, data.data() + src_pos, data_size);
        src_pos += data_size;
        dst_pos += data_size + zero_size;
      }
    } else if (compression == 2) {
      std::vector<uint8_t> packed;
      packed.reserve(data.size());
      const uint8_t* p = data.data();
      const uint8_t* end = data.data() + data.size();
      uint32_t block_size = ffi->compression_info.normal.first_block.block_size;
      while (block_size && p + 24 <= end) {
        const uint8_t* next = p + block_size;
        const uint32_t next_size = Be32(p);  // next block's xex2_compressed_block_info
        p += 24;
        while (p + 2 <= end) {
          const uint32_t chunk = uint32_t(p[0]) << 8 | p[1];
          p += 2;
          if (!chunk) break;
          if (p + chunk > end) break;
          packed.insert(packed.end(), p, p + chunk);
          p += chunk;
        }
        p = next;
        block_size = next_size;
      }
      const uint32_t window = ffi->compression_info.normal.window_size;
      if (lzx_decompress(packed.data(), packed.size(), image.data(), image.size(), window, nullptr,
                         0) != 0) {
        continue;
      }
    } else {
      std::fprintf(stderr, "%s: compression type %u not supported\n", src.c_str(), compression);
      return 1;
    }

    if (image[0] != 'M' || image[1] != 'Z') continue;  // wrong key
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(image.data()), std::streamsize(image.size()));
    std::printf("%s: %s key, compression %u, image 0x%X bytes at 0x%08X -> %s\n", src.c_str(),
                key == kXex2RetailKey ? "retail" : "devkit", compression, image_size, base,
                dst.c_str());
    return out ? 0 : 1;
  }
  std::fprintf(stderr, "%s: no key produced a PE image\n", src.c_str());
  return 1;
}

struct Totals {
  uint64_t files = 0;
  uint64_t bytes = 0;
  uint64_t failures = 0;
};

bool ExtractFile(Entry* entry, const fs::path& dest, std::vector<uint8_t>& buffer) {
  rex::filesystem::File* file = nullptr;
  X_STATUS status = entry->Open(rex::filesystem::FileAccess::kFileReadData, &file);
  if (XFAILED(status) || !file) {
    std::fprintf(stderr, "open failed (0x%08X): %s\n", status, entry->path().c_str());
    return false;
  }
  std::ofstream out(dest, std::ios::binary | std::ios::trunc);
  bool ok = bool(out);
  size_t offset = 0;
  while (ok && offset < entry->size()) {
    size_t want = std::min(buffer.size(), entry->size() - offset);
    size_t got = 0;
    status = file->ReadSync(std::span<uint8_t>(buffer.data(), want), offset, &got);
    if (XFAILED(status) || got == 0) {
      std::fprintf(stderr, "read failed (0x%08X) at %zu: %s\n", status, offset,
                   entry->path().c_str());
      ok = false;
      break;
    }
    out.write(reinterpret_cast<const char*>(buffer.data()), std::streamsize(got));
    ok = bool(out);
    offset += got;
  }
  file->Destroy();
  return ok;
}

void Walk(Entry* dir, const fs::path& out_dir, Totals& totals, std::vector<uint8_t>& buffer) {
  for (const auto& child : dir->children()) {
    fs::path dest = out_dir / child->name();
    if (child->attributes() & rex::filesystem::kFileAttributeDirectory) {
      fs::create_directories(dest);
      Walk(child.get(), dest, totals, buffer);
      continue;
    }
    if (ExtractFile(child.get(), dest, buffer)) {
      totals.files++;
      totals.bytes += child->size();
      std::printf("%12zu  %s\n", child->size(), child->path().c_str());
    } else {
      totals.failures++;
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::string mode = argc == 4 ? argv[1] : "";
  if (mode != "disc" && mode != "stfs" && mode != "image") {
    std::fprintf(stderr, "usage: %s disc|stfs <input> <out_dir>\n       %s image <xex> <out.bin>\n",
                 argv[0], argv[0]);
    return 2;
  }
  const fs::path input = argv[2];
  const fs::path out_dir = argv[3];
  if (mode == "image") return ExtractImage(input, out_dir);

  std::unique_ptr<rex::filesystem::Device> device;
  if (mode == "disc") {
    device = std::make_unique<rex::filesystem::DiscImageDevice>("\\Device\\Cdrom0", input);
  } else {
    device = std::make_unique<rex::filesystem::StfsContainerDevice>("\\Device\\Package", input);
  }
  if (!device->Initialize()) {
    std::fprintf(stderr, "failed to mount %s as %s\n", input.c_str(), mode.c_str());
    return 1;
  }
  Entry* root = device->ResolvePath("");
  if (!root) {
    std::fprintf(stderr, "no root entry in %s\n", input.c_str());
    return 1;
  }

  fs::create_directories(out_dir);
  Totals totals;
  std::vector<uint8_t> buffer(16 << 20);
  Walk(root, out_dir, totals, buffer);
  std::fprintf(stderr, "# %llu files, %llu bytes, %llu failures\n",
               (unsigned long long)totals.files, (unsigned long long)totals.bytes,
               (unsigned long long)totals.failures);
  return totals.failures ? 1 : 0;
}
