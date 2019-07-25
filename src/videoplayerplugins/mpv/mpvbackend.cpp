/*
 * Copyright (C) 2007-2009 Sergio Pistone <sergio_pistone@yahoo.com.ar>
 * Copyright (C) 2010-2019 Mladen Milinkovic <max@smoothware.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "mpvbackend.h"
#include "mpvconfigwidget.h"
#include "mpvconfig.h"

#include <KLocalizedString>

#include <QDebug>

#include <locale>

#include <KMessageBox>

using namespace SubtitleComposer;

MPVBackend::MPVBackend()
	: PlayerBackend(),
	  m_state(STOPPED),
	  m_nativeWindow(nullptr),
	  m_mpv(nullptr),
	  m_initialized(false)
{
	m_name = QStringLiteral("MPV");
	connect(MPVConfig::self(), &MPVConfig::configChanged, this, &MPVBackend::reconfigure);
}

MPVBackend::~MPVBackend()
{
	cleanup();
}

bool
MPVBackend::init(QWidget *videoWidget)
{
	if(!m_nativeWindow) {
		m_nativeWindow = new QWidget(videoWidget);
		m_nativeWindow->setAttribute(Qt::WA_DontCreateNativeAncestors);
		m_nativeWindow->setAttribute(Qt::WA_NativeWindow);
		connect(m_nativeWindow, &QWidget::destroyed, [&](){ m_nativeWindow = nullptr; });
	} else {
		m_nativeWindow->setParent(videoWidget);
	}
	return true;
}

void
MPVBackend::cleanup()
{
	if(m_mpv) {
		mpv_terminate_destroy(m_mpv);
		m_mpv = nullptr;
		m_initialized = false;
		m_state = STOPPED;
	}
}

QWidget *
MPVBackend::newConfigWidget(QWidget *parent)
{
	return new MPVConfigWidget(parent);
}

KCoreConfigSkeleton *
MPVBackend::config() const
{
	return MPVConfig::self();
}

bool
MPVBackend::setup()
{
	// FIXME: libmpv requires LC_NUMERIC category to be set to "C".. is there some nicer way to do this?
	std::setlocale(LC_NUMERIC, "C");

	if(m_mpv)
		mpv_detach_destroy(m_mpv);

	m_mpv = mpv_create();

	if(!m_mpv)
		return false;

	reconfigure();

	// window id
	int64_t winId = m_nativeWindow->winId();
	mpv_set_option(m_mpv, "wid", MPV_FORMAT_INT64, &winId);

	// no OSD
	mpv_set_option_string(m_mpv, "osd-level", "0");

	// Disable subtitles
	mpv_set_option_string(m_mpv, "sid", "no");

	// Start in paused state
	int yes = 1;
	mpv_set_option(m_mpv, "pause", MPV_FORMAT_FLAG, &yes);

	// Disable default bindings
	mpv_set_option_string(m_mpv, "input-default-bindings", "no");
	// Disable keyboard input on the X11 window
	mpv_set_option_string(m_mpv, "input-vo-keyboard", "no");
	// Disable mouse input and let us handle mouse hiding
	mpv_set_option_string(m_mpv, "input-cursor", "no");
	mpv_set_option_string(m_mpv, "cursor-autohide", "no");

	// Receive property change events with MPV_EVENT_PROPERTY_CHANGE
	mpv_observe_property(m_mpv, 0, "time-pos", MPV_FORMAT_DOUBLE);
	mpv_observe_property(m_mpv, 0, "speed", MPV_FORMAT_DOUBLE);
	mpv_observe_property(m_mpv, 0, "volume", MPV_FORMAT_DOUBLE);
	mpv_observe_property(m_mpv, 0, "mute", MPV_FORMAT_FLAG);
	mpv_observe_property(m_mpv, 0, "pause", MPV_FORMAT_FLAG);
	mpv_observe_property(m_mpv, 0, "length", MPV_FORMAT_DOUBLE);
	mpv_observe_property(m_mpv, 0, "track-list", MPV_FORMAT_NODE);

	// Request log messages with level "info" or higher.
	// They are received as MPV_EVENT_LOG_MESSAGE.
	mpv_request_log_messages(m_mpv, "info");

	// From this point on, the wakeup function will be called. The callback
	// can come from any thread, so we use the QueuedConnection mechanism to
	// relay the wakeup in a thread-safe way.
	mpv_set_wakeup_callback(m_mpv, [](void *ctx){
		QMetaObject::invokeMethod(
					reinterpret_cast<MPVBackend *>(ctx),
					&MPVBackend::processEvents, Qt::QueuedConnection);
	}, this);

	m_initialized = mpv_initialize(m_mpv) >= 0;
	m_state = STOPPED;
	return m_initialized;
}

void
MPVBackend::setState(PlayState state)
{
	static VideoPlayer::State vpState[] = { VideoPlayer::Ready, VideoPlayer::Paused, VideoPlayer::Playing };
	if(m_state == state)
		return;
	m_state = state;
	emit stateChanged(vpState[state]);
}

void
MPVBackend::handleEvent(mpv_event *event)
{
	switch(event->event_id) {
	case MPV_EVENT_PROPERTY_CHANGE: {
		mpv_event_property *prop = (mpv_event_property *)event->data;
		const QByteArray name = prop->name;
		if(name == "time-pos") {
			if(prop->format == MPV_FORMAT_DOUBLE) {
				const double time = *reinterpret_cast<double *>(prop->data);
				if(m_state == STOPPED) {
					int paused;
					mpv_get_property(m_mpv, "pause", MPV_FORMAT_FLAG, &paused);
					setState(paused ? PAUSED : PLAYING);
				}
				emit positionChanged(time);
				return;
			}
			if(prop->format == MPV_FORMAT_NONE) {
				// property is unavailable, probably means that playback was stopped.
				setState(STOPPED);
				return;
			}
		}
		if(name == "pause") {
			Q_ASSERT(prop->format == MPV_FORMAT_FLAG);
			const int paused = *reinterpret_cast<int *>(prop->data);
			setState(paused ? PAUSED : PLAYING);
			return;
		}
		if(name == "track-list") {
			notifyAudioStreams(prop);
			notifyTextStreams(prop);
			return;
		}
		if(name == "speed") {
			Q_ASSERT(prop->format == MPV_FORMAT_DOUBLE);
			double rate = *(double *)prop->data;
			emit speedChanged(rate);
			return;
		}
		if(name == "volume") {
			double volumeMax = 100.;
			mpv_get_property(m_mpv, "volume-max", MPV_FORMAT_DOUBLE, &volumeMax);
			Q_ASSERT(prop->format == MPV_FORMAT_DOUBLE);
			double volume = *(double *)prop->data;
			emit volumeChanged(volume * 100. / volumeMax);
			return;
		}
		if(name == "mute") {
			Q_ASSERT(prop->format == MPV_FORMAT_FLAG);
			const int muted = *reinterpret_cast<int *>(prop->data);
			emit muteChanged(muted);
			return;
		}
		return;
	}
	case MPV_EVENT_VIDEO_RECONFIG:
		notifyVideoInfo();
		return;

	case MPV_EVENT_LOG_MESSAGE: {
		struct mpv_event_log_message *msg = (struct mpv_event_log_message *)event->data;
		qDebug("[MPV: %s] %s: %s", msg->prefix, msg->level, QByteArray(msg->text).trimmed().constData());
		if(msg->log_level == MPV_LOG_LEVEL_ERROR && strcmp(msg->prefix, "cplayer") == 0)
			emit errorOccured(QString::fromUtf8(msg->text));
		return;
	}
	case MPV_EVENT_SHUTDOWN: {
		if(m_mpv) {
			mpv_terminate_destroy(m_mpv);
			m_mpv = nullptr;
			m_initialized = false;
		}
		setState(STOPPED);
		return;
	}
	default:
		// Ignore uninteresting or unknown events.
		return;
	}
}

void
MPVBackend::processEvents()
{
	// Process all events, until the event queue is empty.
	while(m_mpv) {
		mpv_event *event = mpv_wait_event(m_mpv, 0);
		if(event->event_id == MPV_EVENT_NONE)
			break;
		handleEvent(event);
	}
}

bool
MPVBackend::openFile(const QString &path)
{
	if(!m_mpv && !setup())
		return false;

	m_currentFilePath = path;
	const QByteArray &filename = m_currentFilePath.toUtf8();
	const char *args[] = { "loadfile", filename.constData(), nullptr };
	mpv_command(m_mpv, args);

	return true;
}

bool
MPVBackend::closeFile()
{
	stop();
	m_currentFilePath.clear();
	return true;
}

bool
MPVBackend::stop()
{
	const char *args[] = { "stop", nullptr };
	mpv_command(m_mpv, args);
	return true;
}

bool
MPVBackend::play()
{
	if(m_initialized == false || m_state == STOPPED) {
		if(!openFile(m_currentFilePath))
			return false;
	}

	if(m_state != PLAYING) {
		const char *args[] = { "cycle", "pause", nullptr };
		mpv_command(m_mpv, args);
	}

	return true;
}

bool
MPVBackend::pause()
{
	const char *args[] = { "cycle", "pause", nullptr };
	mpv_command(m_mpv, args);
	return true;
}

bool
MPVBackend::seek(double seconds)
{
	const QByteArray &timeOffset = QByteArray::number(seconds);
//	const char *args[] = { "seek", timeOffset.constData(), "absolute", "keyframes", nullptr };
	const char *args[] = { "seek", timeOffset.constData(), "absolute", "exact", nullptr };
	mpv_command_async(m_mpv, 0, args);
	return true;
}

bool
MPVBackend::step(int frameOffset)
{
	const char *cmd = frameOffset > 0 ? "frame-step" : "frame-back-step";
	const char *args[] = { cmd, nullptr };
	for(int i = 0, n = qAbs(frameOffset); i < n; i++)
		mpv_command_async(m_mpv, 0, args);
	return true;
}

bool
MPVBackend::playbackRate(double newRate)
{
	if(newRate > 1.) // without frame dropping we might go out of sync
		mpv_set_option_string(m_mpv, "framedrop", "vo");
	else
		mpv_set_option_string(m_mpv, "framedrop", MPVConfig::frameDropping() ? "vo" : "no");
	mpv_set_option(m_mpv, "speed", MPV_FORMAT_DOUBLE, &newRate);
	return true;
}

bool
MPVBackend::selectAudioStream(int streamIndex)
{
	const QByteArray &strIndex = QByteArray::number(streamIndex);
	const char *args[] = { "aid", strIndex.constData(), nullptr };
	mpv_command_async(m_mpv, 0, args);
	return true;
}

bool
MPVBackend::setVolume(double volume)
{
	double volumeMax = 100.;
	mpv_get_property(m_mpv, "volume-max", MPV_FORMAT_DOUBLE, &volumeMax);
	const QByteArray &strVolume = QByteArray::number(volumeMax * volume / 100.);
	const char *args[] = { "set", "volume", strVolume.constData(), nullptr };
	mpv_command_async(m_mpv, 0, args);
	return true;
}

bool
MPVBackend::reconfigure()
{
	if(!m_mpv)
		return false;

	if(MPVConfig::videoOutputEnabled()) {
#if MPV_CLIENT_API_VERSION >= MPV_MAKE_VERSION(1, 21)
		if(MPVConfig::videoOutput() == QStringLiteral("opengl-hq")) {
			mpv_set_option_string(m_mpv, "vo", "opengl");
			mpv_set_option_string(m_mpv, "profile", "opengl-hq");
		} else {
			mpv_set_option_string(m_mpv, "vo", MPVConfig::videoOutput().toUtf8().constData());
		}
#else
		mpv_set_option_string(m_mpv, "vo", MPVConfig::videoOutput().toUtf8().constData());
#endif
	}

	mpv_set_option_string(m_mpv, "hwdec", MPVConfig::hwDecodeEnabled() ? MPVConfig::hwDecode().toUtf8().constData() : "no");

	if(MPVConfig::audioOutputEnabled())
		mpv_set_option_string(m_mpv, "ao", MPVConfig::audioOutput().toUtf8().constData());

	mpv_set_option_string(m_mpv, "audio-channels", MPVConfig::audioChannelsEnabled() ? QByteArray::number(MPVConfig::audioChannels()).constData() : "auto");

	mpv_set_option_string(m_mpv, "framedrop", MPVConfig::frameDropping() ? "vo" : "no");

	if(MPVConfig::autoSyncEnabled())
		mpv_set_option_string(m_mpv, "autosync", QByteArray::number(MPVConfig::autoSyncFactor()).constData());

	if(MPVConfig::cacheEnabled()) {
		mpv_set_option_string(m_mpv, "cache", QByteArray::number(MPVConfig::cacheSize()).constData());
//		mpv_set_option_string(m_mpv, "cache-min", "99");
//		mpv_set_option_string(m_mpv, "cache-seek-min", "99");
	} else {
		mpv_set_option_string(m_mpv, "cache", "auto");
	}

	if(MPVConfig::volumeNormalization())
		mpv_set_option_string(m_mpv, "drc", "1:0.25");

	if(MPVConfig::volumeAmplificationEnabled()) {
#if MPV_CLIENT_API_VERSION >= MPV_MAKE_VERSION(1, 22)
		mpv_set_option_string(m_mpv, "volume-max", QByteArray::number(MPVConfig::volumeAmplification()).constData());
#else
		mpv_set_option_string(m_mpv, "softvol", "yes");
		mpv_set_option_string(m_mpv, "softvol-max", QByteArray::number(MPVConfig::volumeAmplification()).constData());
	} else {
		mpv_set_option_string(m_mpv, "softvol", "no");
#endif
	}

	// restart playing
	if(m_initialized && m_state != STOPPED) {
		bool wasPaused = m_state == PAUSED;
		double oldPosition;
		mpv_get_property(m_mpv, "time-pos", MPV_FORMAT_DOUBLE, &oldPosition);

		stop();
		play();
		seek(oldPosition);
		if(wasPaused)
			pause();
	}

	return true;
}

void
MPVBackend::notifyVideoInfo()
{
	int64_t w, h;
	double dar, fps, length;
	if(mpv_get_property(m_mpv, "dwidth", MPV_FORMAT_INT64, &w) >= 0
			&& mpv_get_property(m_mpv, "dheight", MPV_FORMAT_INT64, &h) >= 0
			&& w > 0 && h > 0) {
		mpv_get_property(m_mpv, "video-aspect", MPV_FORMAT_DOUBLE, &dar);
		emit resolutionChanged(w, h, dar);
	}
	if(mpv_get_property(m_mpv, "estimated-vf-fps", MPV_FORMAT_DOUBLE, &fps) >= 0 && fps > 0)
		emit fpsChanged(fps);
	else if(mpv_get_property(m_mpv, "container-fps", MPV_FORMAT_DOUBLE, &fps) >= 0 && fps > 0)
		emit fpsChanged(fps);
	if(mpv_get_property(m_mpv, "duration", MPV_FORMAT_DOUBLE, &length) >= 0 && length > 0)
		emit lengthChanged(length);
}

static QVariant
node_to_variant(const mpv_node *node)
{
	switch(node->format) {
	case MPV_FORMAT_STRING:
		return QVariant(QString::fromUtf8(node->u.string));
	case MPV_FORMAT_FLAG:
		return QVariant(static_cast<bool>(node->u.flag));
	case MPV_FORMAT_INT64:
		return QVariant(static_cast<qlonglong>(node->u.int64));
	case MPV_FORMAT_DOUBLE:
		return QVariant(node->u.double_);
	case MPV_FORMAT_NODE_ARRAY: {
		mpv_node_list *list = node->u.list;
		QVariantList qlist;
		for(int n = 0; n < list->num; n++)
			qlist.append(node_to_variant(&list->values[n]));
		return QVariant(qlist);
	}
	case MPV_FORMAT_NODE_MAP: {
		mpv_node_list *list = node->u.list;
		QVariantMap qmap;
		for(int n = 0; n < list->num; n++)
			qmap.insert(QString::fromUtf8(list->keys[n]), node_to_variant(&list->values[n]));
		return QVariant(qmap);
	}
	default: // MPV_FORMAT_NONE, unknown values (e.g. future extensions)
		return QVariant();
	}
}

void
MPVBackend::notifyTextStreams(const mpv_event_property *prop)
{
	QStringList textStreams;
	if(prop->format == MPV_FORMAT_NODE) {
		const mpv_node *node = (mpv_node *)prop->data;
		if(node->format == MPV_FORMAT_NODE_ARRAY) {
			for(int i = 0; i < node->u.list->num; i++) {
				const mpv_node &val = node->u.list->values[i];
				if(val.format != MPV_FORMAT_NODE_MAP)
					continue;

				const QMap<QString, QVariant> &map = node_to_variant(&val).toMap();

				if(map[QStringLiteral("type")].toString() != QStringLiteral("sub")
				|| map[QStringLiteral("external")].toBool() == true)
					continue;

				const QString &codec = map[QStringLiteral("codec")].toString();
				if(codec != QStringLiteral("mov_text") && codec != QStringLiteral("subrip"))
					continue;

				const int &id = map[QStringLiteral("id")].toInt();
				const QString &lang = map[QStringLiteral("lang")].toString();
				const QString &title = map[QStringLiteral("title")].toString();

				QString textStreamName = i18n("Text Stream #%1", id);
				if(!lang.isEmpty() && lang != QStringLiteral("und"))
					textStreamName += QStringLiteral(": ") + lang;
				if(!title.isEmpty())
					textStreamName += QStringLiteral(": ") + title;

				textStreams << textStreamName;
			}
		}
	}
	emit textStreamsChanged(textStreams);
}

void
MPVBackend::notifyAudioStreams(const mpv_event_property *prop)
{
	QStringList audioStreams;
	if(prop->format == MPV_FORMAT_NODE) {
		const mpv_node *node = (mpv_node *)prop->data;
		if(node->format == MPV_FORMAT_NODE_ARRAY) {
			for(int i = 0; i < node->u.list->num; i++) {
				const mpv_node &val = node->u.list->values[i];
				if(val.format != MPV_FORMAT_NODE_MAP)
					continue;

				const QMap<QString, QVariant> &map = node_to_variant(&val).toMap();

				if(map[QStringLiteral("type")].toString() != QStringLiteral("audio"))
					continue;

				const int &id = map[QStringLiteral("id")].toInt();
				const QString &lang = map[QStringLiteral("lang")].toString();
				const QString &title = map[QStringLiteral("title")].toString();
				const QString &codec = map[QStringLiteral("codec")].toString();

				QString audioStreamName = i18n("Audio Stream #%1", id);
				if(!lang.isEmpty() && lang != QStringLiteral("und"))
					audioStreamName += QStringLiteral(": ") + lang;
				if(!title.isEmpty())
					audioStreamName += QStringLiteral(": ") + title;
				if(!codec.isEmpty())
					audioStreamName += QStringLiteral(" [") + codec + QStringLiteral("]");

				audioStreams << audioStreamName;
			}
		}
	}
	emit audioStreamsChanged(audioStreams, audioStreams.isEmpty() ? -1 : 0);
}
