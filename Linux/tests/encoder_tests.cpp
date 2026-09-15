#include "opendisplay/ffmpeg_encoder.hpp"
#include "opendisplay/wire.hpp"

#include <unistd.h>

#include <array>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool hasOnlyFourByteStartCodes(const std::string_view bytes) {
    for (std::size_t index = 0; index + 2 < bytes.size(); ++index) {
        if (bytes[index] == 0 && bytes[index + 1] == 0 && bytes[index + 2] == 1
            && (index == 0 || bytes[index - 1] != 0)) {
            return false;
        }
    }
    return true;
}

bool hasNalType(const std::string_view bytes, const unsigned char type) {
    for (std::size_t index = 0; index + 4 < bytes.size(); ++index) {
        if (bytes[index] == 0 && bytes[index + 1] == 0 && bytes[index + 2] == 0
            && bytes[index + 3] == 1
            && (static_cast<unsigned char>(bytes[index + 4]) & 0x1fU) == type) {
            return true;
        }
    }
    return false;
}

od::CapturedFrame testFrame(const int index) {
    od::CapturedFrame frame;
    frame.format = od::VideoFormat{.width = 64, .height = 64, .stride = 256, .fps = 30};
    frame.capturedAtMs = 1000 + index;
    frame.sequence = static_cast<unsigned>(index);
    frame.bytes.assign(64 * 64 * 4, static_cast<char>(index * 20));
    return frame;
}

// Decodes the delivered stream with ffprobe and returns its pixel format.
std::string probePixelFormat(const std::vector<od::EncodedFrame>& frames) {
    char path[] = "/tmp/opendisplay-encoder-test-XXXXXX.h264";
    const int fd = ::mkstemps(path, 5);
    assert(fd >= 0);
    ::close(fd);
    {
        std::ofstream file(path, std::ios::binary);
        for (const auto& frame : frames) {
            file.write(frame.annexB.data(), static_cast<std::streamsize>(frame.annexB.size()));
        }
    }
    const std::string command = "ffprobe -v error -select_streams v:0 -show_entries "
                                "stream=pix_fmt -of csv=p=0 " + std::string(path);
    FILE* process = ::popen(command.c_str(), "r");
    assert(process != nullptr);
    std::array<char, 256> buffer{};
    std::string output;
    while (::fgets(buffer.data(), static_cast<int>(buffer.size()), process) != nullptr) {
        output.append(buffer.data());
    }
    ::pclose(process);
    ::unlink(path);
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
        output.pop_back();
    }
    return output;
}

}  // namespace

int main() {
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<od::EncodedFrame> output;
    od::FfmpegEncoder encoder;
    encoder.start(od::EncoderConfig{
        .kind = od::EncoderKind::Software,
        .outputWidth = 64,
        .outputHeight = 64,
        .fps = 30,
        .bitrate = 300'000,
    }, [&](od::EncodedFrame frame) {
        {
            std::lock_guard lock(mutex);
            output.push_back(std::move(frame));
        }
        condition.notify_all();
    });

    // Every frame must be delivered on its own, before the next one is
    // submitted and without stopping the encoder to flush it.
    constexpr int frameCount = 8;
    for (int index = 0; index < frameCount; ++index) {
        encoder.submit(testFrame(index));
        std::unique_lock lock(mutex);
        const bool delivered = condition.wait_for(lock, std::chrono::seconds(5), [&] {
            return output.size() > static_cast<std::size_t>(index);
        });
        assert(delivered);
        assert(output.size() == static_cast<std::size_t>(index) + 1);
        assert(output.back().capturedAtMs == 1000 + index);
    }
    encoder.stop();
    assert(output.size() == frameCount);

    assert(od::wire::containsAnnexBStartCode(output.front().annexB));
    assert(output.front().keyframe);
    assert(hasNalType(output.front().annexB, 7));  // SPS
    assert(hasNalType(output.front().annexB, 8));  // PPS
    assert(hasNalType(output.front().annexB, 5));  // IDR slice
    for (std::size_t index = 1; index < output.size(); ++index) {
        assert(!output[index].keyframe);
        assert(hasNalType(output[index].annexB, 1));  // non-IDR slice
    }
    for (const auto& encoded : output) {
        assert(hasNalType(encoded.annexB, 9));  // AUD
        assert(hasOnlyFourByteStartCodes(encoded.annexB));
    }
    // iPad hardware decoders need 8-bit 4:2:0, not the 4:4:4 libx264 picks for RGB.
    assert(probePixelFormat(output) == "yuv420p");
    return 0;
}
