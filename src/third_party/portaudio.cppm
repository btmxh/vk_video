module;

#include <portaudiocpp/PortAudioCpp.hxx>

export module vkvideo.third_party:portaudio;

export namespace vkvideo::tp::portaudio {
using AutoSystem = ::portaudio::AutoSystem;
using System = ::portaudio::System;
using Device = ::portaudio::Device;
using Stream = ::portaudio::Stream;
using StreamParameters = ::portaudio::StreamParameters;
using DirectionSpecificStreamParameters =
    ::portaudio::DirectionSpecificStreamParameters;
using SampleDataFormat = ::portaudio::SampleDataFormat;
template <class T>
using MemFunCallbackStream = ::portaudio::MemFunCallbackStream<T>;
using FunCallbackStream = ::portaudio::FunCallbackStream;
using StreamCallbackFlags = ::PaStreamCallbackFlags;
using StreamCallbackTimeInfo = ::PaStreamCallbackTimeInfo;
} // namespace vkvideo::tp::portaudio
