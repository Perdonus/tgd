/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_transcribes.h"

#include "apiwrap.h"
#include "api/api_text_entities.h"
#include "data/data_channel.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_helpers.h"
#include "lang/lang_keys.h"
#include "logs.h"
#include "main/main_app_config.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "media/audio/media_audio_local_cache.h"
#include "settings.h"
#include "spellcheck/spellcheck_types.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLocale>
#include <QtCore/QObject>
#include <QtCore/QProcess>
#include <QtCore/QTemporaryDir>

#include <array>

namespace {

struct LocalSpeechModelSpec {
	QString folderName;
	QString culture;
};

struct LocalSpeechRunResult {
	bool success = false;
	QString text;
	QString error;
	QString culture;
	QString recognizer;
};

[[nodiscard]] const std::array<LocalSpeechModelSpec, 12> &LocalSpeechModels() {
	static const auto kModels = std::array<LocalSpeechModelSpec, 12>{{
		{ u"vosk-model-small-ru-0.22"_q, u"ru-RU"_q },
		{ u"vosk-model-small-en-us-0.15"_q, u"en-US"_q },
		{ u"vosk-model-small-uk-v3-small"_q, u"uk-UA"_q },
		{ u"vosk-model-small-de-0.15"_q, u"de-DE"_q },
		{ u"vosk-model-small-fr-0.22"_q, u"fr-FR"_q },
		{ u"vosk-model-small-es-0.42"_q, u"es-ES"_q },
		{ u"vosk-model-small-it-0.22"_q, u"it-IT"_q },
		{ u"vosk-model-small-pt-0.3"_q, u"pt-PT"_q },
		{ u"vosk-model-small-tr-0.3"_q, u"tr-TR"_q },
		{ u"vosk-model-small-pl-0.22"_q, u"pl-PL"_q },
		{ u"vosk-model-small-ja-0.22"_q, u"ja-JP"_q },
		{ u"vosk-model-small-kz-0.42"_q, u"kk-KZ"_q },
	}};
	return kModels;
}

[[nodiscard]] QString NormalizeCulture(QString value) {
	value = value.trimmed();
	return value.replace(u'_', u'-');
}

[[nodiscard]] QString LocalSpeechModelsDir() {
	return QDir(cWorkingDir()).filePath(u"tdata/speech_models"_q);
}

[[nodiscard]] QString LocalSpeechToolsDir() {
	return QDir(cWorkingDir()).filePath(u"tdata/bin"_q);
}

[[nodiscard]] QString LocalSpeechScriptPath() {
	return QDir(LocalSpeechToolsDir()).filePath(
		u"astro_local_transcribe.ps1"_q);
}

[[nodiscard]] QStringList InstalledLocalSpeechCultures() {
#ifdef Q_OS_WIN
	const auto modelsDir = LocalSpeechModelsDir();
	auto result = QStringList();
	for (const auto &spec : LocalSpeechModels()) {
		const auto path = QDir(modelsDir).filePath(spec.folderName);
		if (QFileInfo(path).isDir()) {
			result.push_back(spec.culture);
		}
	}
	result.removeDuplicates();
	return result;
#else // Q_OS_WIN
	return {};
#endif // Q_OS_WIN
}

[[nodiscard]] QStringList OrderLocalSpeechCultures(QStringList cultures) {
	cultures.removeDuplicates();
	if (cultures.isEmpty()) {
		return cultures;
	}
	const auto locale = NormalizeCulture(
		QLocale::system().bcp47Name().isEmpty()
			? QLocale::system().name()
			: QLocale::system().bcp47Name());
	const auto prefix = locale.section(u'-', 0, 0);
	auto result = QStringList();
	const auto pushMatch = [&](Fn<bool(const QString&)> predicate) {
		for (const auto &culture : cultures) {
			if (predicate(culture) && !result.contains(culture)) {
				result.push_back(culture);
			}
		}
	};
	pushMatch([&](const QString &culture) {
		return NormalizeCulture(culture).compare(locale, Qt::CaseInsensitive) == 0;
	});
	pushMatch([&](const QString &culture) {
		return NormalizeCulture(culture).section(u'-', 0, 0).compare(
			prefix,
			Qt::CaseInsensitive) == 0;
	});
	for (const auto &culture : cultures) {
		if (!result.contains(culture)) {
			result.push_back(culture);
		}
	}
	return result;
}

[[nodiscard]] QString LocalSpeechScriptContents() {
	return QString::fromUtf8(R"PS1(
param(
  [Parameter(Mandatory = $true)][string]$InputPath,
  [string]$Cultures = ""
)

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$ErrorActionPreference = 'Stop'

function Write-Result([bool]$Success, [string]$Text, [string]$Error, [string]$Culture, [string]$Recognizer) {
  @{
    success = $Success
    text = $Text
    error = $Error
    culture = $Culture
    recognizer = $Recognizer
  } | ConvertTo-Json -Compress
}

try {
  Add-Type -AssemblyName System.Speech
  $available = [System.Speech.Recognition.SpeechRecognitionEngine]::InstalledRecognizers()
  if ($available.Count -eq 0) {
    Write-Result $false "" "No installed Windows speech recognizers were found." "" ""
    exit 2
  }

  $requested = @()
  if ($Cultures) {
    $requested = $Cultures.Split(';') |
      Where-Object { $_ -and $_.Trim().Length -gt 0 } |
      ForEach-Object { $_.Trim() }
  }

  $selected = New-Object 'System.Collections.Generic.List[System.Speech.Recognition.RecognizerInfo]'
  foreach ($culture in $requested) {
    $normalized = $culture.Replace('_', '-')
    $prefix = $normalized.Split('-')[0].ToLowerInvariant()
    foreach ($info in ($available | Where-Object { $_.Culture.Name -ieq $normalized })) {
      if (-not $selected.Contains($info)) { [void]$selected.Add($info) }
    }
    foreach ($info in ($available | Where-Object { $_.Culture.TwoLetterISOLanguageName -ieq $prefix })) {
      if (-not $selected.Contains($info)) { [void]$selected.Add($info) }
    }
  }
  if (($requested.Count -gt 0) -and ($selected.Count -eq 0)) {
    Write-Result $false "" ("No matching Windows speech recognizer is installed for: " + ($requested -join ', ')) "" ""
    exit 4
  }
  if ($selected.Count -eq 0) {
    foreach ($info in $available) {
      if (-not $selected.Contains($info)) { [void]$selected.Add($info) }
    }
  }

  $bestText = ""
  $bestCulture = ""
  $bestRecognizer = ""
  $errors = New-Object 'System.Collections.Generic.List[string]'

  foreach ($info in $selected) {
    $engine = $null
    try {
      $engine = New-Object System.Speech.Recognition.SpeechRecognitionEngine($info)
      $engine.InitialSilenceTimeout = [TimeSpan]::FromSeconds(30)
      $engine.BabbleTimeout = [TimeSpan]::FromSeconds(10)
      $engine.EndSilenceTimeout = [TimeSpan]::FromSeconds(1)
      $engine.EndSilenceTimeoutAmbiguous = [TimeSpan]::FromSeconds(1)
      $grammar = New-Object System.Speech.Recognition.DictationGrammar
      $engine.LoadGrammar($grammar)
      $engine.SetInputToWaveFile($InputPath)
      $builder = New-Object System.Text.StringBuilder
      while ($true) {
        $result = $engine.Recognize()
        if ($null -eq $result) { break }
        if ($result.Text) {
          if ($builder.Length -gt 0) { [void]$builder.Append(' ') }
          [void]$builder.Append($result.Text)
        }
      }
      $text = $builder.ToString().Trim()
      if ($text.Length -gt $bestText.Length) {
        $bestText = $text
        $bestCulture = $info.Culture.Name
        $bestRecognizer = $info.Name
      }
    } catch {
      [void]$errors.Add(($info.Culture.Name + ': ' + $_.Exception.Message))
    } finally {
      if ($engine -ne $null) {
        $engine.Dispose()
      }
    }
  }

  if ([string]::IsNullOrWhiteSpace($bestText)) {
    $message = if ($errors.Count -gt 0) { ($errors -join ' | ') } else { 'Speech recognition returned empty text.' }
    Write-Result $false "" $message $bestCulture $bestRecognizer
    exit 3
  }

  Write-Result $true $bestText "" $bestCulture $bestRecognizer
  exit 0
} catch {
  Write-Result $false "" $_.Exception.Message "" ""
  exit 1
}
)PS1");
}

[[nodiscard]] bool EnsureLocalSpeechScript(QString *error) {
#ifdef Q_OS_WIN
	QDir().mkpath(LocalSpeechToolsDir());
	const auto path = LocalSpeechScriptPath();
	const auto contents = LocalSpeechScriptContents().toUtf8();
	auto existing = QFile(path);
	if (existing.open(QIODevice::ReadOnly)) {
		const auto current = existing.readAll();
		existing.close();
		if (current == contents) {
			return true;
		}
	}
	auto file = QFile(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		if (error) {
			*error = QString::fromLatin1("Could not create local transcribe script: %1")
				.arg(path);
		}
		return false;
	}
	if (file.write(contents) != contents.size()) {
		file.close();
		if (error) {
			*error = QString::fromLatin1("Could not write local transcribe script: %1")
				.arg(path);
		}
		return false;
	}
	file.close();
	return true;
#else // Q_OS_WIN
	if (error) {
		*error = QString::fromLatin1("Local speech transcription is only available on Windows.");
	}
	return false;
#endif // Q_OS_WIN
}

[[nodiscard]] LocalSpeechRunResult RunLocalSpeechTranscription(
		const QString &wavPath,
		const QStringList &cultures) {
	auto result = LocalSpeechRunResult();
#ifdef Q_OS_WIN
	auto scriptError = QString();
	if (!EnsureLocalSpeechScript(&scriptError)) {
		result.error = scriptError;
		return result;
	}
	auto process = QProcess();
	process.setProgram(u"powershell.exe"_q);
	process.setArguments({
		u"-NoProfile"_q,
		u"-ExecutionPolicy"_q,
		u"Bypass"_q,
		u"-File"_q,
		LocalSpeechScriptPath(),
		u"-InputPath"_q,
		wavPath,
		u"-Cultures"_q,
		cultures.join(u';'),
	});
	process.start();
	if (!process.waitForStarted(5000)) {
		result.error = QString::fromLatin1("Could not start powershell.exe: %1")
			.arg(process.errorString());
		return result;
	}
	if (!process.waitForFinished(10 * 60 * 1000)) {
		process.kill();
		process.waitForFinished();
		result.error = QString::fromLatin1("Local speech transcription timed out.");
		return result;
	}
	const auto stdoutData = process.readAllStandardOutput();
	const auto stderrData = process.readAllStandardError();
	const auto parsed = QJsonDocument::fromJson(stdoutData);
	if (parsed.isObject()) {
		const auto object = parsed.object();
		result.success = object.value(u"success"_q).toBool(false);
		result.text = object.value(u"text"_q).toString();
		result.error = object.value(u"error"_q).toString();
		result.culture = object.value(u"culture"_q).toString();
		result.recognizer = object.value(u"recognizer"_q).toString();
	}
	if (!result.success && result.error.isEmpty()) {
		result.error = !stderrData.trimmed().isEmpty()
			? QString::fromUtf8(stderrData).trimmed()
			: !stdoutData.trimmed().isEmpty()
			? QString::fromUtf8(stdoutData).trimmed()
			: QString::fromLatin1("Local speech transcription failed.");
	}
#else // Q_OS_WIN
	Q_UNUSED(wavPath);
	Q_UNUSED(cultures);
	result.error = QString::fromLatin1(
		"Local speech transcription is only available on Windows.");
#endif // Q_OS_WIN
	return result;
}

[[nodiscard]] QString FriendlyLocalSpeechError(const QString &error) {
	if (error.contains(u"No installed Windows speech recognizers"_q, Qt::CaseInsensitive)) {
		return QObject::tr(
			"Windows speech recognition is not installed for the selected language.");
	} else if (error.contains(u"No matching Windows speech recognizer"_q, Qt::CaseInsensitive)) {
		return QObject::tr(
			"Install the matching Windows speech recognition pack for the downloaded language.");
	} else if (error.contains(u"empty text"_q, Qt::CaseInsensitive)) {
		return QObject::tr("The local recognizer returned empty text.");
	} else if (error.contains(u"timed out"_q, Qt::CaseInsensitive)) {
		return QObject::tr("The local recognizer timed out.");
	}
	return error;
}

} // namespace

namespace Api {

Transcribes::Transcribes(not_null<ApiWrap*> api)
: _session(&api->session())
, _api(&api->instance()) {
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
	if (const auto channel = item->history()->peer->asMegagroup()) {
		const auto owner = &channel->owner();
		return channel->levelHint() >= owner->groupFreeTranscribeLevel();
	}
	return false;
}

bool Transcribes::localAvailable(not_null<HistoryItem*> item) const {
#ifdef Q_OS_WIN
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	if (!document || (!document->isVoiceMessage() && !document->isVideoMessage())) {
		return false;
	}
	return !InstalledLocalSpeechCultures().isEmpty();
#else // Q_OS_WIN
	Q_UNUSED(item);
	return false;
#endif // Q_OS_WIN
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
		localAvailable(item) ? loadLocal(item) : load(item);
		_session->data().requestItemResize(item);
	} else if (!i->second.requestId) {
		if (i->second.failed || i->second.toolong) {
			localAvailable(item) ? loadLocal(item) : load(item);
			_session->data().requestItemResize(item);
			return;
		}
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
		entry.error = QString();
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
		entry.error = QString();
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
	entry.toolong = false;
	entry.pending = false;
	entry.error = QString();
	entry.result = QString();
}

void Transcribes::loadLocal(not_null<HistoryItem*> item) {
	if (!item->isHistoryEntry() || item->isLocal()) {
		return;
	}
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	if (!document || (!document->isVoiceMessage() && !document->isVideoMessage())) {
		return;
	}

	const auto id = item->fullId();
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
	const auto requestId = ++_nextLocalRequestId;
	const auto cultures = OrderLocalSpeechCultures(InstalledLocalSpeechCultures());
	const auto bytes = [&] {
		if (const auto active = document->activeMediaView()) {
			const auto cached = active->bytes();
			if (!cached.isEmpty()) {
				return cached;
			}
		}
		return QByteArray();
	}();
	const auto filePath = document->filepath(true);

	auto &entry = _map[id];
	entry.requestId = requestId;
	entry.shown = true;
	entry.failed = false;
	entry.toolong = false;
	entry.pending = true;
	entry.error = QString();
	entry.result = QString();
	toggleRound(item, entry);
	_session->data().requestItemResize(item);

	if (cultures.isEmpty()) {
		entry.requestId = 0;
		entry.pending = false;
		entry.failed = true;
		entry.error = QObject::tr(
			"Install at least one local speech language first.");
		_session->data().requestItemResize(item);
		return;
	}
	if (bytes.isEmpty() && filePath.isEmpty()) {
		entry.requestId = 0;
		entry.pending = false;
		entry.failed = true;
		entry.error = QObject::tr(
			"Download the voice message first and try again.");
		_session->data().requestItemResize(item);
		return;
	}

		Logs::writeClient(QString::fromLatin1(
			"[local-transcribe] started item=%1:%2 cultures=%3")
			.arg(QString::number(id.peer.value))
			.arg(QString::number(id.msg.bare))
			.arg(cultures.join(u","_q)));

	crl::async([=, session = _session] {
		auto result = LocalSpeechRunResult();
		QDir().mkpath(cWorkingDir() + u"tdata/temp"_q);
		auto tempDir = QTemporaryDir(
			QDir(cWorkingDir()).filePath(u"tdata/temp/astro-transcribe-XXXXXX"_q));
		if (!tempDir.isValid()) {
			result.error = QString::fromLatin1(
				"Could not create a temporary directory for local transcription.");
		} else {
			auto sourceBytes = bytes;
			if (sourceBytes.isEmpty() && !filePath.isEmpty()) {
				auto file = QFile(filePath);
				if (file.open(QIODevice::ReadOnly)) {
					sourceBytes = file.readAll();
				}
			}
			if (sourceBytes.isEmpty()) {
				result.error = QString::fromLatin1(
					"Source audio bytes are not available yet.");
			} else {
				const auto wav = Media::Audio::ToSpeechWav(sourceBytes);
				if (wav.isEmpty()) {
					result.error = QString::fromLatin1(
						"Could not convert the voice message to WAV.");
				} else {
					const auto wavPath = QDir(tempDir.path()).filePath(
						u"input.wav"_q);
					auto wavFile = QFile(wavPath);
					if (!wavFile.open(QIODevice::WriteOnly | QIODevice::Truncate)
						|| wavFile.write(wav) != wav.size()) {
						result.error = QString::fromLatin1(
							"Could not write the local transcription WAV file.");
					} else {
						wavFile.close();
						result = RunLocalSpeechTranscription(wavPath, cultures);
					}
				}
			}
		}
		crl::on_main([=] {
			const auto current = session->data().message(id);
			auto i = _map.find(id);
			if (i == _map.end() || (i->second.requestId != requestId)) {
				return;
			}
			auto &entry = i->second;
			entry.requestId = 0;
			entry.pending = false;
			entry.result = result.success ? result.text.trimmed() : QString();
			entry.error = result.success
				? QString()
				: FriendlyLocalSpeechError(result.error).trimmed();
			entry.failed = !result.success;
			entry.toolong = false;
			if (current) {
				if (entry.roundview) {
					session->data().requestItemViewRefresh(current);
				}
				session->data().requestItemResize(current);
			}
				Logs::writeClient(QString::fromLatin1(
					"[local-transcribe] finished item=%1:%2 success=%3 culture=%4 recognizer=%5 error=%6")
					.arg(QString::number(id.peer.value))
					.arg(QString::number(id.msg.bare))
					.arg(result.success ? u"true"_q : u"false"_q)
					.arg(result.culture)
				.arg(result.recognizer)
				.arg(entry.error));
		});
	});
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
