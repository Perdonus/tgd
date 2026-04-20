/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_transcribes.h"

#include "apiwrap.h"
#include "api/api_text_entities.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "data/data_channel.h"
#include "data/data_document.h"
#include "data/data_file_origin.h"
#include "data/data_document_media.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_helpers.h"
#include "lang/lang_keys.h"
#include "lang/lang_instance.h"
#include "logs.h"
#include "main/main_app_config.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "media/audio/media_audio_ffmpeg_loader.h"
#include "spellcheck/spellcheck_types.h"
#include "storage/localstorage.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QTemporaryFile>

#include <cmath>

namespace {

[[nodiscard]] QString RuEn(const char *ru, const char *en) {
	return Lang::GetInstance().id().startsWith(u"ru"_q, Qt::CaseInsensitive)
		? QString::fromUtf8(ru)
		: QString::fromUtf8(en);
}

[[nodiscard]] QString LocalSpeechPreparingText(bool roundVideo) {
	return roundVideo
		? RuEn(
			"Подготавливаю кружочек для локального распознавания. Кнопку можно нажать ещё раз, как только медиа догрузится.",
			"Preparing the video note for local transcription. Tap the button again once the media finishes downloading.")
		: RuEn(
			"Подготавливаю аудио для локального распознавания. Кнопку можно нажать ещё раз, как только файл догрузится.",
			"Preparing audio for local transcription. Tap the button again once the file finishes downloading.");
}

constexpr auto kLocalSpeechTargetRate = 16000;
[[nodiscard]] QString SpeechModelsRootPath() {
	return QDir(cWorkingDir()).filePath(u"tdata/speech_models"_q);
}

[[nodiscard]] QString SpeechRuntimeRootPath() {
	return QDir(cWorkingDir()).filePath(u"tdata/speech_runtime/vosk"_q);
}

[[nodiscard]] QStringList InstalledSpeechModelFolders() {
	return QDir(SpeechModelsRootPath()).entryList(
		QDir::Dirs | QDir::NoDotAndDotDot,
		QDir::Name | QDir::IgnoreCase);
}

[[nodiscard]] int ChannelsFromOpenAlFormat(int format) {
	switch (format) {
	case AL_FORMAT_STEREO8:
	case AL_FORMAT_STEREO16:
		return 2;
	case AL_FORMAT_MONO8:
	case AL_FORMAT_MONO16:
	default:
		return 1;
	}
}

[[nodiscard]] int BitsFromOpenAlFormat(int format) {
	switch (format) {
	case AL_FORMAT_MONO8:
	case AL_FORMAT_STEREO8:
		return 8;
	case AL_FORMAT_MONO16:
	case AL_FORMAT_STEREO16:
	default:
		return 16;
	}
}

void AppendWaveLE16(not_null<QByteArray*> buffer, int value) {
	buffer->push_back(char(value & 0xFF));
	buffer->push_back(char((value >> 8) & 0xFF));
}

void AppendWaveLE32(not_null<QByteArray*> buffer, int value) {
	buffer->push_back(char(value & 0xFF));
	buffer->push_back(char((value >> 8) & 0xFF));
	buffer->push_back(char((value >> 16) & 0xFF));
	buffer->push_back(char((value >> 24) & 0xFF));
}

[[nodiscard]] std::vector<int16> DecodeMonoSamples(
		const QByteArray &raw,
		int format) {
	const auto channels = std::max(ChannelsFromOpenAlFormat(format), 1);
	const auto bits = BitsFromOpenAlFormat(format);
	const auto bytesPerSample = std::max(bits / 8, 1);
	const auto frameSize = std::max(bytesPerSample * channels, 1);
	const auto frames = raw.size() / frameSize;
	auto result = std::vector<int16>(frames);
	const auto data = reinterpret_cast<const uchar*>(raw.constData());
	for (auto frame = 0; frame != frames; ++frame) {
		auto mixed = 0;
		for (auto channel = 0; channel != channels; ++channel) {
			const auto offset = (frame * frameSize) + (channel * bytesPerSample);
			if (bits == 8) {
				mixed += (int(data[offset]) - 128) << 8;
			} else {
				const auto lo = quint16(data[offset]);
				const auto hi = quint16(data[offset + 1]) << 8;
				mixed += qint16(lo | hi);
			}
		}
		result[frame] = qBound(-32768, mixed / channels, 32767);
	}
	return result;
}

[[nodiscard]] std::vector<int16> ResampleMonoTo16k(
		const std::vector<int16> &input,
		int sourceRate) {
	if (input.empty() || (sourceRate <= 0) || (sourceRate == kLocalSpeechTargetRate)) {
		return input;
	}
	const auto targetFrames = std::max(
		1,
		int(std::llround(
			(double(input.size()) * kLocalSpeechTargetRate) / sourceRate)));
	auto result = std::vector<int16>(targetFrames);
	const auto ratio = double(sourceRate) / kLocalSpeechTargetRate;
	for (auto i = 0; i != targetFrames; ++i) {
		const auto src = i * ratio;
		const auto left = std::clamp(int(std::floor(src)), 0, int(input.size()) - 1);
		const auto right = std::clamp(left + 1, 0, int(input.size()) - 1);
		const auto mix = src - left;
		const auto value = int(std::lround(
			(input[left] * (1. - mix)) + (input[right] * mix)));
		result[i] = qBound(-32768, value, 32767);
	}
	return result;
}

[[nodiscard]] QByteArray BuildMonoWavePcm16(
		const std::vector<int16> &samples,
		int sampleRate) {
	auto result = QByteArray();
	const auto dataSize = int(samples.size() * sizeof(int16));
	result.reserve(44 + dataSize);
	result += "RIFF";
	AppendWaveLE32(&result, 36 + dataSize);
	result += "WAVEfmt ";
	AppendWaveLE32(&result, 16);
	AppendWaveLE16(&result, 1);
	AppendWaveLE16(&result, 1);
	AppendWaveLE32(&result, sampleRate);
	AppendWaveLE32(&result, sampleRate * int(sizeof(int16)));
	AppendWaveLE16(&result, sizeof(int16));
	AppendWaveLE16(&result, 16);
	result += "data";
	AppendWaveLE32(&result, dataSize);
	result.append(
		reinterpret_cast<const char*>(samples.data()),
		dataSize);
	return result;
}

[[nodiscard]] QByteArray DecodeWaveForLocalSpeech(
		const Core::FileLocation &file,
		const QByteArray &data,
		QString *error) {
	auto loader = Media::FFMpegLoader(
		file,
		data,
		bytes::vector());
	if (!loader.open(0)) {
		if (error) {
			*error = RuEn(
				"Не удалось открыть аудиофайл для локального распознавания.",
				"Could not open the audio file for local transcription.");
		}
		return {};
	}
	auto decoded = QByteArray();
	for (;;) {
		const auto chunk = loader.readMore();
		if (const auto readError = std::get_if<Media::AudioPlayerLoader::ReadError>(&chunk)) {
			switch (*readError) {
			case Media::AudioPlayerLoader::ReadError::Retry:
			case Media::AudioPlayerLoader::ReadError::RetryNotQueued:
			case Media::AudioPlayerLoader::ReadError::Wait:
				continue;
			case Media::AudioPlayerLoader::ReadError::EndOfFile:
				break;
			case Media::AudioPlayerLoader::ReadError::Other:
				if (error) {
					*error = RuEn(
						"Не удалось декодировать аудиофайл для локального распознавания.",
						"Could not decode the audio file for local transcription.");
				}
				return {};
			}
			break;
		}
		const auto span = std::get<bytes::const_span>(chunk);
		decoded.append(
			reinterpret_cast<const char*>(span.data()),
			int(span.size()));
	}
	if (decoded.isEmpty()) {
		if (error) {
			*error = RuEn(
				"Локально распознать нечего: аудиоданные пустые.",
				"There is nothing to transcribe locally: audio data is empty.");
		}
		return {};
	}
	const auto mono = DecodeMonoSamples(decoded, loader.format());
	const auto resampled = ResampleMonoTo16k(mono, loader.samplesFrequency());
	if (resampled.empty()) {
		if (error) {
			*error = RuEn(
				"Не удалось подготовить PCM для локального распознавания.",
				"Could not prepare PCM data for local transcription.");
		}
		return {};
	}
	return BuildMonoWavePcm16(resampled, kLocalSpeechTargetRate);
}

[[nodiscard]] QByteArray LocalSpeechPowerShellScript() {
	return QByteArray(R"PS(
param(
  [Parameter(Mandatory=$true)][string]$AudioPath,
  [Parameter(Mandatory=$true)][string]$ModelPath,
  [Parameter(Mandatory=$true)][string]$RuntimeRoot
)
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
try {
  Add-Type -AssemblyName System.IO.Compression.FileSystem | Out-Null
  if (-not (Test-Path -LiteralPath $AudioPath)) {
    throw 'Audio payload for local transcription was not found.'
  }
  if (-not (Test-Path -LiteralPath $ModelPath)) {
    throw 'The selected Vosk model was not found.'
  }
  $managedDll = Join-Path $RuntimeRoot 'lib\netstandard2.0\Vosk.dll'
  $nativeDir = Join-Path $RuntimeRoot 'build\lib\win-x64'
  $nativeDll = Join-Path $nativeDir 'libvosk.dll'
  if ((-not (Test-Path -LiteralPath $managedDll)) -or (-not (Test-Path -LiteralPath $nativeDll))) {
    if (Test-Path -LiteralPath $RuntimeRoot) {
      Remove-Item -LiteralPath $RuntimeRoot -Recurse -Force
    }
    $parent = Split-Path -Parent $RuntimeRoot
    if (-not [string]::IsNullOrWhiteSpace($parent)) {
      New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    $package = Join-Path ([System.IO.Path]::GetTempPath()) ('astrogram-vosk-' + [Guid]::NewGuid().ToString('N') + '.nupkg')
    try {
      Invoke-WebRequest -UseBasicParsing -Uri 'https://www.nuget.org/api/v2/package/Vosk/0.3.38' -OutFile $package
      [System.IO.Compression.ZipFile]::ExtractToDirectory($package, $RuntimeRoot)
    } finally {
      if (Test-Path -LiteralPath $package) {
        Remove-Item -LiteralPath $package -Force
      }
    }
  }
  if ((-not (Test-Path -LiteralPath $managedDll)) -or (-not (Test-Path -LiteralPath $nativeDll))) {
    throw 'The local Vosk runtime is incomplete after extraction.'
  }
  $env:PATH = $nativeDir + ';' + $env:PATH
  [System.Reflection.Assembly]::LoadFrom($managedDll) | Out-Null
  [Vosk.Vosk]::SetLogLevel(-1)
  $model = [Vosk.Model]::new($ModelPath)
  $recognizer = [Vosk.VoskRecognizer]::new($model, 16000.0)
  $parts = New-Object System.Collections.Generic.List[string]
  $stream = [System.IO.File]::OpenRead($AudioPath)
  try {
    if ($stream.Length -gt 44) {
      $stream.Position = 44
    }
    $buffer = New-Object byte[] 4000
    while (($read = $stream.Read($buffer, 0, $buffer.Length)) -gt 0) {
      if ($recognizer.AcceptWaveform($buffer, $read)) {
        $chunk = $null
        try {
          $chunk = $recognizer.Result() | ConvertFrom-Json
        } catch {
          $chunk = $null
        }
        if (($null -ne $chunk) -and (-not [string]::IsNullOrWhiteSpace($chunk.text))) {
          $parts.Add(([string]$chunk.text).Trim())
        }
      }
    }
    $final = $null
    try {
      $final = $recognizer.FinalResult() | ConvertFrom-Json
    } catch {
      $final = $null
    }
    if (($null -ne $final) -and (-not [string]::IsNullOrWhiteSpace($final.text))) {
      $parts.Add(([string]$final.text).Trim())
    }
  } finally {
    if ($null -ne $stream) {
      $stream.Dispose()
    }
    if ($null -ne $recognizer -and $recognizer -is [System.IDisposable]) {
      $recognizer.Dispose()
    }
    if ($null -ne $model -and $model -is [System.IDisposable]) {
      $model.Dispose()
    }
  }
  @{
    ok = $true
    text = ($parts -join ' ')
  } | ConvertTo-Json -Compress
} catch {
  @{
    ok = $false
    error = $_.Exception.Message
  } | ConvertTo-Json -Compress
  exit 1
}
)PS");
}

[[nodiscard]] QString BestInstalledSpeechModelPath() {
	const auto installed = InstalledSpeechModelFolders();
	if (installed.isEmpty()) {
		return QString();
	}
	const auto modelsRoot = QDir(SpeechModelsRootPath());
	const auto findByTokens = [&](const QStringList &tokens) -> QString {
		for (const auto &folder : installed) {
			const auto value = folder.toLower();
			for (const auto &token : tokens) {
				if (value.contains(token)) {
					return modelsRoot.filePath(folder);
				}
			}
		}
		return QString();
	};
	const auto ui = Lang::LanguageIdOrDefault(Lang::Id()).toLower();
	const auto choose = [&](const QString &prefix, const QStringList &tokens) {
		return ui.startsWith(prefix) ? findByTokens(tokens) : QString();
	};
	if (const auto path = choose(u"ru"_q, { u"-ru-"_q, u"-ru"_q }); !path.isEmpty()) {
		return path;
	} else if (const auto path = choose(u"uk"_q, { u"-uk-"_q, u"-uk"_q }); !path.isEmpty()) {
		return path;
	} else if (const auto path = choose(u"de"_q, { u"-de-"_q, u"-de"_q }); !path.isEmpty()) {
		return path;
	} else if (const auto path = choose(u"fr"_q, { u"-fr-"_q, u"-fr"_q }); !path.isEmpty()) {
		return path;
	} else if (const auto path = choose(u"es"_q, { u"-es-"_q, u"-es"_q }); !path.isEmpty()) {
		return path;
	} else if (const auto path = choose(u"pt"_q, { u"-pt-"_q, u"-pt"_q }); !path.isEmpty()) {
		return path;
	} else if (const auto path = choose(u"it"_q, { u"-it-"_q, u"-it"_q }); !path.isEmpty()) {
		return path;
	} else if (const auto path = choose(u"nl"_q, { u"-nl-"_q, u"-nl"_q }); !path.isEmpty()) {
		return path;
	} else if (const auto path = choose(u"pl"_q, { u"-pl-"_q, u"-pl"_q }); !path.isEmpty()) {
		return path;
	} else if (const auto path = choose(u"tr"_q, { u"-tr-"_q, u"-tr"_q }); !path.isEmpty()) {
		return path;
	} else if (const auto path = choose(u"sv"_q, { u"-sv-"_q, u"-sv"_q }); !path.isEmpty()) {
		return path;
	} else if (const auto path = choose(u"cs"_q, { u"-cs-"_q, u"-cs"_q }); !path.isEmpty()) {
		return path;
	} else if (const auto path = choose(u"ja"_q, { u"-ja-"_q, u"-ja"_q }); !path.isEmpty()) {
		return path;
	} else if (const auto path = choose(u"ko"_q, { u"-ko-"_q, u"-ko"_q }); !path.isEmpty()) {
		return path;
	} else if (const auto path = choose(u"zh"_q, { u"-cn-"_q, u"-zh"_q }); !path.isEmpty()) {
		return path;
	} else if (const auto path = choose(u"hi"_q, { u"-hi-"_q, u"-hi"_q }); !path.isEmpty()) {
		return path;
	} else if (const auto path = choose(u"fa"_q, { u"-fa-"_q, u"-fa"_q }); !path.isEmpty()) {
		return path;
	} else if (const auto path = choose(u"vi"_q, { u"-vn-"_q, u"-vi"_q }); !path.isEmpty()) {
		return path;
	} else if (const auto path = choose(u"en"_q, { u"-en-"_q, u"-en"_q }); !path.isEmpty()) {
		return path;
	}
	return modelsRoot.filePath(installed.front());
}

} // namespace

namespace Api {

Transcribes::Transcribes(not_null<ApiWrap*> api)
: _session(&api->session())
, _api(&api->instance()) {
	crl::on_main(_session, [=] {
		_session->data().itemDataChanges(
		) | rpl::on_next([=](not_null<HistoryItem*> item) {
			maybeRetryPendingLocal(item);
		}, _lifetime);
	});
}

Transcribes::~Transcribes() = default;

bool Transcribes::localModeEnabled() const {
	return Core::App().settings().localSpeechRecognition();
}

bool Transcribes::isRated(not_null<HistoryItem*> item) const {
	const auto fullId = item->fullId();
	for (const auto &[transcribeId, id] : _ids) {
		if (id == fullId) {
			return _session->settings().isTranscriptionRated(transcribeId);
		}
	}
	return false;
}

void Transcribes::rate(not_null<HistoryItem*> item, bool isGood) {
	const auto fullId = item->fullId();
	for (const auto &[transcribeId, id] : _ids) {
		if (id == fullId) {
			_api.request(MTPmessages_RateTranscribedAudio(
				item->history()->peer->input(),
				MTP_int(item->id),
				MTP_long(transcribeId),
				MTP_bool(isGood))).send();
			_session->settings().markTranscriptionAsRated(transcribeId);
			_session->saveSettings();
			return;
		}
	}
}

bool Transcribes::freeFor(not_null<HistoryItem*> item) const {
	if (localModeEnabled()) {
		return true;
	}
	if (const auto channel = item->history()->peer->asMegagroup()) {
		const auto owner = &channel->owner();
		return channel->levelHint() >= owner->groupFreeTranscribeLevel();
	}
	return false;
}

bool Transcribes::trialsSupport() {
	if (!_trialsSupport) {
		const auto count = _session->appConfig().get<int>(
			u"transcribe_audio_trial_weekly_number"_q,
			0);
		const auto until = _session->appConfig().get<int>(
			u"transcribe_audio_trial_cooldown_until"_q,
			0);
		_trialsSupport = (count > 0) || (until > 0);
	}
	return *_trialsSupport;
}

TimeId Transcribes::trialsRefreshAt() {
	if (_trialsRefreshAt < 0) {
		_trialsRefreshAt = _session->appConfig().get<int>(
			u"transcribe_audio_trial_cooldown_until"_q,
			0);
	}
	return _trialsRefreshAt;
}

int Transcribes::trialsCount() {
	if (_trialsCount < 0) {
		_trialsCount = _session->appConfig().get<int>(
			u"transcribe_audio_trial_weekly_number"_q,
			-1);
		return std::max(_trialsCount, 0);
	}
	return _trialsCount;
}

crl::time Transcribes::trialsMaxLengthMs() const {
	return 1000 * _session->appConfig().get<int>(
		u"transcribe_audio_trial_duration_max"_q,
		300);
}

void Transcribes::toggle(not_null<HistoryItem*> item) {
	const auto id = item->fullId();
	auto i = _map.find(id);
	if (i == _map.end()) {
		load(item);
		_session->data().requestItemResize(item);
		if (localModeEnabled()) {
			_session->data().requestItemViewRefresh(item);
		}
	} else if (localModeEnabled()
		&& !i->second.requestId
		&& (i->second.pending || i->second.failed)) {
		loadLocal(item);
		_session->data().requestItemResize(item);
		_session->data().requestItemViewRefresh(item);
	} else if (!i->second.requestId) {
		i->second.shown = !i->second.shown;
		if (i->second.roundview) {
			_session->data().requestItemViewRefresh(item);
		}
		_session->data().requestItemResize(item);
	}
}

void Transcribes::toggleSummary(
		not_null<HistoryItem*> item,
		Fn<void()> onPremiumRequired) {
	const auto id = item->fullId();
	auto i = _summaries.find(id);
	if (i == _summaries.end()) {
		auto &entry = _summaries.emplace(id).first->second;
		entry.onPremiumRequired = std::move(onPremiumRequired);
		summarize(item);
	} else if (!i->second.loading) {
		auto &entry = i->second;
		if (entry.result.empty()) {
			entry.onPremiumRequired = std::move(onPremiumRequired);
			summarize(item);
		} else {
			entry.shown = entry.premiumRequired ? false : !entry.shown;
			_session->data().requestItemResize(item);
			if (entry.shown) {
				_session->data().requestItemShowHighlight(item);
			}
		}
	}
}

const Transcribes::Entry &Transcribes::entry(
		not_null<HistoryItem*> item) const {
	static auto empty = Entry();
	const auto i = _map.find(item->fullId());
	return (i != _map.end()) ? i->second : empty;
}

const SummaryEntry &Transcribes::summary(
		not_null<const HistoryItem*> item) const {
	static const auto empty = SummaryEntry();
	const auto i = _summaries.find(item->fullId());
	return (i != _summaries.end()) ? i->second : empty;
}

void Transcribes::apply(const MTPDupdateTranscribedAudio &update) {
	const auto id = update.vtranscription_id().v;
	const auto i = _ids.find(id);
	if (i == _ids.end()) {
		return;
	}
	const auto j = _map.find(i->second);
	if (j == _map.end()) {
		return;
	}
	const auto text = qs(update.vtext());
	j->second.result = text;
	j->second.pending = update.is_pending();
	if (const auto item = _session->data().message(i->second)) {
		if (j->second.roundview) {
			_session->data().requestItemViewRefresh(item);
		}
		_session->data().requestItemResize(item);
	}
}

void Transcribes::load(not_null<HistoryItem*> item) {
	if (localModeEnabled()) {
		loadLocal(item);
		return;
	}
	if (!item->isHistoryEntry() || item->isLocal()) {
		return;
	}
	const auto toggleRound = [](not_null<HistoryItem*> item, Entry &entry) {
		if (const auto media = item->media()) {
			if (const auto document = media->document()) {
				if (document->isVideoMessage()) {
					entry.roundview = true;
					document->owner().requestItemViewRefresh(item);
				}
			}
		}
	};
	const auto id = item->fullId();
	const auto requestId = _api.request(MTPmessages_TranscribeAudio(
		item->history()->peer->input(),
		MTP_int(item->id)
	)).done([=](const MTPmessages_TranscribedAudio &result) {
		const auto &data = result.data();

		{
			const auto trialsCountChanged = data.vtrial_remains_num()
				&& (_trialsCount != data.vtrial_remains_num()->v);
			if (trialsCountChanged) {
				_trialsCount = data.vtrial_remains_num()->v;
			}
			const auto refreshAtChanged = data.vtrial_remains_until_date()
				&& (_trialsRefreshAt != data.vtrial_remains_until_date()->v);
			if (refreshAtChanged) {
				_trialsRefreshAt = data.vtrial_remains_until_date()->v;
			}
			if (trialsCountChanged) {
				ShowTrialTranscribesToast(_trialsCount, _trialsRefreshAt);
			}
		}

		auto &entry = _map[id];
		entry.requestId = 0;
		entry.pending = data.is_pending();
		entry.result = qs(data.vtext());
		_ids.emplace(data.vtranscription_id().v, id);
		if (const auto item = _session->data().message(id)) {
			toggleRound(item, entry);
			_session->data().requestItemResize(item);
		}
	}).fail([=](const MTP::Error &error) {
		auto &entry = _map[id];
		entry.requestId = 0;
		entry.pending = false;
		entry.failed = true;
		if (error.type() == u"MSG_VOICE_TOO_LONG"_q) {
			entry.toolong = true;
		}
		if (const auto item = _session->data().message(id)) {
			toggleRound(item, entry);
			_session->data().requestItemResize(item);
		}
	}).send();
	auto &entry = _map.emplace(id).first->second;
	entry.requestId = requestId;
	entry.shown = true;
	entry.failed = false;
	entry.pending = false;
}

void Transcribes::loadLocal(not_null<HistoryItem*> item) {
	const auto id = item->fullId();
	auto &entry = _map.emplace(id).first->second;
	if (entry.requestId) {
		return;
	}
	entry.shown = true;
	entry.failed = false;
	entry.pending = false;
	entry.toolong = false;
	entry.result.clear();
	entry.requestId = 1;

#ifndef Q_OS_WIN
	entry.requestId = 0;
	entry.failed = true;
	entry.result = RuEn(
		"Локальное распознавание речи сейчас доступно только в Windows-сборке Astrogram.",
		"Local speech recognition is currently available only in the Windows build of Astrogram.");
	_session->data().requestItemResize(item);
	return;
#else // Q_OS_WIN
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	const auto mediaView = document ? document->activeMediaView() : nullptr;
	auto path = document ? document->filepath(true) : QString();
	auto bytes = mediaView ? mediaView->bytes() : QByteArray();
	if (path.isEmpty() && document && !bytes.isEmpty()) {
		document->saveFromDataSilent();
		path = document->filepath(true);
	}
	if (document && document->isVideoMessage()) {
		entry.roundview = true;
		_session->data().requestItemViewRefresh(item);
	}
	if (path.isEmpty() && bytes.isEmpty()) {
		if (document) {
			if (document->isVideoMessage()) {
				document->forceToCache(true);
			}
			document->save(item->fullId(), QString(), LoadFromCloudOrLocal, true);
		} else if (mediaView) {
			mediaView->automaticLoad(item->fullId(), item);
		}
		entry.requestId = 0;
		entry.pending = true;
		entry.failed = false;
		entry.result = LocalSpeechPreparingText(
			document && document->isVideoMessage());
		_session->data().requestItemResize(item);
		_session->data().requestItemViewRefresh(item);
		return;
	}
	entry.pending = false;

	const auto modelPath = BestInstalledSpeechModelPath();
	if (modelPath.isEmpty()) {
		entry.requestId = 0;
		entry.failed = true;
		entry.result = RuEn(
			"Сначала скачайте хотя бы одну Vosk-модель в настройках Astrogram.",
			"Download at least one Vosk model in Astrogram settings first.");
		_session->data().requestItemResize(item);
		_session->data().requestItemViewRefresh(item);
		return;
	}

	auto decodeError = QString();
	const auto wave = DecodeWaveForLocalSpeech(
		path.isEmpty() ? Core::FileLocation() : Core::FileLocation(path),
		bytes,
		&decodeError);
	if (wave.isEmpty()) {
		entry.requestId = 0;
		entry.failed = true;
		entry.result = decodeError.isEmpty()
			? RuEn(
				"Не удалось подготовить аудио для локального распознавания.",
				"Could not prepare audio for local transcription.")
			: decodeError;
		_session->data().requestItemResize(item);
		_session->data().requestItemViewRefresh(item);
		return;
	}

	auto wavFile = QTemporaryFile();
	wavFile.setAutoRemove(false);
	wavFile.setFileTemplate(
		QDir(QDir::tempPath()).filePath(u"astrogram-local-transcribe-XXXXXX.wav"_q));
	if (!wavFile.open() || (wavFile.write(wave) != wave.size())) {
		entry.requestId = 0;
		entry.failed = true;
		entry.result = RuEn(
			"Не удалось записать временный WAV для локального распознавания.",
			"Could not write a temporary WAV file for local transcription.");
		_session->data().requestItemResize(item);
		_session->data().requestItemViewRefresh(item);
		return;
	}
	const auto wavPath = wavFile.fileName();
	wavFile.close();

	auto scriptFile = QTemporaryFile();
	scriptFile.setAutoRemove(false);
	scriptFile.setFileTemplate(
		QDir(QDir::tempPath()).filePath(u"astrogram-local-transcribe-XXXXXX.ps1"_q));
	if (!scriptFile.open()
		|| (scriptFile.write(LocalSpeechPowerShellScript())
			!= LocalSpeechPowerShellScript().size())) {
		QFile::remove(wavPath);
		entry.requestId = 0;
		entry.failed = true;
		entry.result = RuEn(
			"Не удалось записать PowerShell-скрипт локального распознавания.",
			"Could not write the local transcription PowerShell script.");
		_session->data().requestItemResize(item);
		_session->data().requestItemViewRefresh(item);
		return;
	}
	const auto scriptPath = scriptFile.fileName();
	scriptFile.close();

	auto process = std::make_unique<QProcess>();
	process->setProgram(u"powershell"_q);
	auto arguments = QStringList{
		u"-NoProfile"_q,
		u"-NonInteractive"_q,
		u"-ExecutionPolicy"_q,
		u"Bypass"_q,
		u"-File"_q,
		scriptPath,
		u"-AudioPath"_q,
		wavPath,
		u"-ModelPath"_q,
		modelPath,
		u"-RuntimeRoot"_q,
		SpeechRuntimeRootPath(),
	};
	process->setArguments(arguments);
	process->setProcessChannelMode(QProcess::SeparateChannels);
	auto *raw = process.get();
	entry.pending = true;
	entry.result = RuEn(
		"Подготавливаю локальный движок и запускаю Vosk-распознавание...",
		"Preparing the local runtime and starting Vosk transcription...");
	QObject::connect(
		raw,
		&QProcess::errorOccurred,
		raw,
		[this, id, wavPath, scriptPath](QProcess::ProcessError error) {
			const auto i = _localProcesses.find(id);
			if (i == _localProcesses.end()) {
				QFile::remove(wavPath);
				QFile::remove(scriptPath);
				return;
			}
			auto process = std::move(i->second);
			_localProcesses.erase(i);
			QFile::remove(wavPath);
			QFile::remove(scriptPath);
			auto &entry = _map[id];
			entry.requestId = 0;
			entry.pending = false;
			entry.failed = true;
			entry.result = RuEn(
				"Не удалось запустить локальный Vosk runner (%1).",
				"Could not start the local Vosk runner (%1).")
					.arg(QString::number(int(error)));
			if (const auto updated = _session->data().message(id)) {
				_session->data().requestItemResize(updated);
				_session->data().requestItemViewRefresh(updated);
			}
		});
	QObject::connect(
		raw,
		qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
		raw,
		[this, id, wavPath, scriptPath](int exitCode, QProcess::ExitStatus exitStatus) {
			const auto it = _localProcesses.find(id);
			if (it == _localProcesses.end()) {
				QFile::remove(wavPath);
				QFile::remove(scriptPath);
				return;
			}
			auto process = std::move(it->second);
			_localProcesses.erase(it);
			const auto output = QString::fromUtf8(
				process->readAllStandardOutput()).trimmed();
			const auto errorOutput = QString::fromUtf8(
				process->readAllStandardError()).trimmed();
			QFile::remove(wavPath);
			QFile::remove(scriptPath);

			auto text = QString();
			auto failed = (exitStatus != QProcess::NormalExit) || (exitCode != 0);
			if (!output.isEmpty()) {
				const auto json = QJsonDocument::fromJson(output.toUtf8());
				if (json.isObject()) {
					const auto object = json.object();
					failed = !object.value(u"ok"_q).toBool(false);
					text = failed
						? object.value(u"error"_q).toString().trimmed()
						: object.value(u"text"_q).toString().trimmed();
				} else {
					text = output;
				}
			}
			if (text.isEmpty()) {
				text = failed
					? (!errorOutput.isEmpty()
						? errorOutput
						: RuEn(
							"Локальное распознавание завершилось ошибкой.",
							"Local transcription failed."))
					: RuEn(
						"Локальное распознавание завершилось без текста.",
						"Local transcription finished without any text.");
			}

			auto &entry = _map[id];
			entry.requestId = 0;
			entry.pending = false;
			entry.failed = failed;
			entry.result = text;
			if (const auto updated = _session->data().message(id)) {
				_session->data().requestItemResize(updated);
				_session->data().requestItemViewRefresh(updated);
			}
		});
	_localProcesses.emplace(id, std::move(process));
	raw->start();
	_session->data().requestItemResize(item);
	_session->data().requestItemViewRefresh(item);
#endif // Q_OS_WIN
}

void Transcribes::maybeRetryPendingLocal(not_null<HistoryItem*> item) {
	if (!localModeEnabled() || !item->isHistoryEntry() || item->isLocal()) {
		return;
	}
	const auto i = _map.find(item->fullId());
	if (i == _map.end()) {
		return;
	}
	const auto &entry = i->second;
	if (!entry.pending || entry.requestId) {
		return;
	}
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	if (!document) {
		return;
	}
	const auto mediaView = document->activeMediaView();
	auto path = document->filepath(true);
	auto bytes = mediaView ? mediaView->bytes() : QByteArray();
	if (path.isEmpty() && !bytes.isEmpty()) {
		document->saveFromDataSilent();
		path = document->filepath(true);
	}
	if (path.isEmpty() && bytes.isEmpty()) {
		return;
	}
	loadLocal(item);
	_session->data().requestItemResize(item);
	_session->data().requestItemViewRefresh(item);
}

void Transcribes::summarize(not_null<HistoryItem*> item) {
	if (!item->isHistoryEntry() || item->isLocal()) {
		return;
	}

	const auto id = item->fullId();
	const auto translatedTo = item->history()->translatedTo();
	const auto langCode = translatedTo
		? translatedTo.twoLetterCode()
		: QString();
	const auto requestId = _api.request(MTPmessages_SummarizeText(
		langCode.isEmpty()
			? MTP_flags(0)
			: MTP_flags(MTPmessages_summarizeText::Flag::f_to_lang),
		item->history()->peer->input(),
		MTP_int(item->id),
		langCode.isEmpty() ? MTPstring() : MTP_string(langCode)
	)).done([=](const MTPTextWithEntities &result) {
		const auto &data = result.data();
		auto &entry = _summaries[id];
		entry.requestId = 0;
		entry.loading = false;
		entry.premiumRequired = false;
		entry.onPremiumRequired = nullptr;
		entry.languageId = translatedTo;
		entry.result = TextWithEntities(
			qs(data.vtext()),
			Api::EntitiesFromMTP(_session, data.ventities().v));
		if (const auto item = _session->data().message(id)) {
			_session->data().requestItemTextRefresh(item);
			_session->data().requestItemShowHighlight(item);
		}
	}).fail([=](const MTP::Error &error) {
		auto &entry = _summaries[id];
		if (error.type() == u"SUMMARY_FLOOD_PREMIUM"_q) {
			if (!entry.premiumRequired && entry.onPremiumRequired) {
				entry.onPremiumRequired();
			}
			entry.premiumRequired = true;
		}
		entry.requestId = 0;
		entry.shown = false;
		entry.loading = false;
		entry.onPremiumRequired = nullptr;
		if (const auto item = _session->data().message(id)) {
			_session->data().requestItemTextRefresh(item);
		}
	}).send();

	auto &entry = _summaries.emplace(id).first->second;
	entry.requestId = requestId;
	entry.shown = true;
	entry.loading = true;

	item->setHasSummaryEntry();
	_session->data().requestItemResize(item);
}

void Transcribes::checkSummaryToTranslate(FullMsgId id) {
	const auto i = _summaries.find(id);
	if (i == _summaries.end() || i->second.result.empty()) {
		return;
	}
	const auto item = _session->data().message(id);
	if (!item) {
		return;
	}
	const auto translatedTo = item->history()->translatedTo();
	if (i->second.languageId != translatedTo) {
		i->second.result = tr::lng_contacts_loading(tr::now, tr::italic);
		summarize(item);
	}
}

} // namespace Api
