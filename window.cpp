#include <QGuiApplication>
#include <QScreen>
#include <QEvent>
#include <QCloseEvent>
#include <QTimer>
#include <QLayout>
#include <QGridLayout>
#include <QDateTime>
#include <QTimeZone>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <chrono>
#include <stdexcept>
#include "window.h"
#include "receivers.h"
#include "recognizers.h"

std::chrono::milliseconds Window::uptime=TimeConvert::Now();
const char *Window::SETTINGS_CATEGORY_VIBE="Vibe";
const char *Window::SETTINGS_CATEGORY_WINDOW="Window";
const char *Window::SETTINGS_CATEGORY_EVENTS="Events";

/*!
 * \class Window
 * \brief The top most and only window of the application.
 *
 * This isn't meant to contain any visible content other than a sequence
 * of panes.
 */

/*!
 * \fn Window::Window
 * \brief Constructs the main window, initializing any settings, media players,
 * and timers needed for built-in commands.
 */
Window::Window() : QWidget(nullptr),
	channel(new Channel(this)),
	visiblePane(nullptr),
	chatPane(new ChatPane(this)),
	vibeFader(nullptr),
	background(new QWidget(this)),
	vibeKeeper(new QMediaPlayer(this)),
	roaster(new QMediaPlayer(this)),
	settingWindowSize(SETTINGS_CATEGORY_WINDOW,"Size"),
	settingHelpCooldown(SETTINGS_CATEGORY_WINDOW,"HelpCooldown",300000),
	settingInactivityCooldown(SETTINGS_CATEGORY_WINDOW,"InactivityCooldown",1800000),
	settingVibePlaylist(SETTINGS_CATEGORY_VIBE,"Playlist"),
	settingRoasts(SETTINGS_CATEGORY_EVENTS,"Roasts"),
	settingBackgroundColor(SETTINGS_CATEGORY_WINDOW,"BackgroundColor","#ff000000"),
	settingAccentColor(SETTINGS_CATEGORY_WINDOW,"AccentColor","#ff000000"),
	settingArrivalSound(SETTINGS_CATEGORY_EVENTS,"Arrival"),
	settingSubscriptionSound(SETTINGS_CATEGORY_EVENTS,"Subscription"),
	settingCheerSound(SETTINGS_CATEGORY_EVENTS,"Cheer"),
	settingRaidSound(SETTINGS_CATEGORY_EVENTS,"Raid"),
	settingRaidInterruptDuration(SETTINGS_CATEGORY_EVENTS,"RaidInterruptDelay",60000),
	settingPortraitVideo(SETTINGS_CATEGORY_EVENTS,"Portrait")
{
	setAttribute(Qt::WA_TranslucentBackground,true);
	if (settingWindowSize)
		setFixedSize(settingWindowSize);
	else
		setFixedSize(ScreenThird());

	setLayout(new QGridLayout(this));
	layout()->setContentsMargins(0,0,0,0);
	background->setLayout(new QGridLayout(background)); // for translucency to work, there has to be a widget covering the window, otherwise the entire thing is clear
	background->layout()->setContentsMargins(0,0,0,0);
	QColor backgroundColor=settingBackgroundColor;
	background->setStyleSheet(QString("background-color: rgba(%1,%2,%3,%4);").arg(
		StringConvert::Integer(backgroundColor.red()),
		StringConvert::Integer(backgroundColor.green()),
		StringConvert::Integer(backgroundColor.blue()),
		StringConvert::Integer(backgroundColor.alpha())));
	layout()->addWidget(background);

	SwapPane(new StatusPane(this));

	helpClock.setInterval(TimeConvert::Interval(std::chrono::milliseconds(settingHelpCooldown)));
	inactivityClock.setInterval(TimeConvert::Interval(std::chrono::milliseconds(settingInactivityCooldown)));
	lastRaid=QDateTime::currentDateTime().addMSecs(static_cast<qint64>(0)-static_cast<qint64>(settingRaidInterruptDuration));
	vibeKeeper->setVolume(0);

	connect(this,&Window::RequestEphemeralPane,this,&Window::StageEphemeralPane);
}

/*!
 * \fn Window::event
 * \brief Handles events the window receives from the operating system.
 *
 * This is where the process of connecting to IRC is started. Last minute
 * initialization is also performed here.
 *
 * \param event Pointer to the QEvent that has been occurred
 * \return bool Whether or not the event was successfully processed
 */
bool Window::event(QEvent *event)
{
	if (event->type() == QEvent::Polish)
	{
		const char *OPERATION="Initialize";
		try
		{
			emit Print(Console::GenerateMessage(QCoreApplication::applicationName(),OPERATION,QString("Creating data directory %1").arg(Filesystem::DataPath().absolutePath())));
			if (!Filesystem::DataPath().exists() && !Filesystem::DataPath().mkpath(Filesystem::DataPath().absolutePath())) throw std::runtime_error("Failed to create data directory");
			emit Print(Console::GenerateMessage(QCoreApplication::applicationName(),OPERATION,QString("Creating log directory %1").arg(Filesystem::LogPath().absolutePath())));
			connect(this,&Window::Print,this,&Window::Log);

			BuildEventSubscriber();

			connect(channel,QOverload<ChatMessageReceiver*>::of(&Channel::Joined),this,&Window::FollowChat);
			connect(channel,&Channel::Ping,[this]() {
				if (settingPortraitVideo)
					StageEphemeralPane(new VideoPane(settingPortraitVideo));
				else
					chatPane->Alert("Twitch is asking if we're still here<br>Letting Twitch server know we're still here");
			});
			connect(channel,&Channel::Print,this,&Window::Print);
			connect(channel,&Channel::Log,this,&Window::Log);
			channel->Connect();
		}

		catch (std::runtime_error &exception)
		{
			emit Print(Console::GenerateMessage(QCoreApplication::applicationName(),OPERATION,QString("Aborting initialization!\n%1").arg(exception.what())));
		}
	}

	return QWidget::event(event);
}

void Window::BuildEventSubscriber()
{
	UserRecognizer *userRecognizer=new UserRecognizer(channel->Name());
	connect(userRecognizer,&UserRecognizer::Recognized,[this](UserRecognizer* recognizer) {
		EventSubscriber* subscriber=CreateEventSubscriber(recognizer->Name());
		emit Print(Console::GenerateMessage(QCoreApplication::applicationName(),"eventsub",QString("Event subscriber created for user ID: %1").arg(recognizer->ID())));
		connect(subscriber, &EventSubscriber::Print, this, &Window::Log);
		subscriber->Listen();
		subscriber->Subscribe(SUBSCRIPTION_TYPE_FOLLOW);
		subscriber->Subscribe(SUBSCRIPTION_TYPE_REDEMPTION);
		subscriber->Subscribe(SUBSCRIPTION_TYPE_CHEER);
		subscriber->Subscribe(SUBSCRIPTION_TYPE_RAID);
		subscriber->Subscribe(SUBSCRIPTION_TYPE_SUBSCRIPTION);
		connect(subscriber,&EventSubscriber::Redemption,[this](const QString& viewer,const QString& rewardTitle,const QString& message) {
			StageEphemeralPane(new AnnouncePane({
				{QString("%1<br>").arg(viewer),1.5},
				{"has redeemed<br>",1},
				{QString("%1<br>").arg(rewardTitle),1.5},
				{message,1}
			}));
		});
		connect(subscriber,&EventSubscriber::Raid,[this](const QString& viewer, const unsigned int viewers) {
			try
			{
				StageEphemeralPane(new AudioAnnouncePane({
					{QString("%1<br>").arg(viewer),1.5},
					{"is raiding with<br>",1},
					{QString("%1<br>").arg(StringConvert::PositiveInteger(viewers)),1.5},
					{"viewers",1}
				},settingRaidSound));
				lastRaid=QDateTime::currentDateTime();
			}
			catch (std::runtime_error &exception)
			{
				chatPane->Alert("Failed to create pane for raid");
				emit Log(exception.what());
			}
		});
		connect(subscriber,&EventSubscriber::Subscription,[this](const QString& viewer) {
			StageEphemeralPane(new AudioAnnouncePane({
				{QString("%1<br>").arg(viewer),1.5},
				{"has subscribed!",1}
			},settingSubscriptionSound));
		});
		connect(subscriber,&EventSubscriber::Cheer,[this](const QString &viewer,const unsigned int count,const QString &message) {
			StageEphemeralPane(new AudioAnnouncePane({
				{QString("%1<br>").arg(viewer),1.5},
				{"has cheered<br>",1},
				{QString("%1<br>").arg(message),1.5},
				{QString("for %1 bits").arg(QString::number(count)),0.8}
			},settingCheerSound));
		});
		recognizer->deleteLater();
	});
	connect(userRecognizer,&UserRecognizer::Error,[this](const QString &message) {
		emit Print(Console::GenerateMessage(QCoreApplication::applicationName(),"recognize user",message));
	});
	// FIXME: there is a (ridiculously small) window of opportunity here for the network call to succeed before the
	// connections are made
}

/*!
 * \fn Window::SwapPane
 * \brief Changes the visible pane to a new pane
 *
 * This will also deallocate the previously visible pane
 *
 * \param pane The pane to make visible
 */
void Window::SwapPane(PersistentPane *pane)
{
	if (visiblePane) visiblePane->deleteLater();
	visiblePane=pane;
	background->layout()->addWidget(visiblePane);
	connect(this,&Window::Print,visiblePane,&PersistentPane::Print);
}

/*!
 * \fn Window::FollowChat
 * \brief Watch for and process chat messages
 *
 * This is where the bot will spend most of its time. When a message is
 * received, it is processed and sent to the correct subsystem if
 * recognized. This is also where any persistent operations, such as
 * periodically displaying command hints, are started.
 */
void Window::FollowChat(ChatMessageReceiver *chatMessageReceiver)
{
	connect(chatMessageReceiver,&ChatMessageReceiver::Refresh,chatPane,&ChatPane::Refresh);
	connect(chatMessageReceiver,&ChatMessageReceiver::Alert,chatPane,&ChatPane::Alert);
	connect(chatMessageReceiver,&ChatMessageReceiver::Print,chatPane,&ChatPane::Print);
	if (settingArrivalSound) connect(chatMessageReceiver,&ChatMessageReceiver::ArrivalConfirmed,this,&Window::AnnounceArrival);
	connect(chatMessageReceiver,&ChatMessageReceiver::PlayVideo,[this](const QString &path) {
		StageEphemeralPane(new VideoPane(path));
	});
	connect(chatMessageReceiver,&ChatMessageReceiver::PlayAudio,[this](const QString &user,const QString &message,const QString &path) {
		StageEphemeralPane(new AudioAnnouncePane({
			{QString("%1<br>").arg(user),1.5},
			{message,1}
		},path));
	});
	connect(chatMessageReceiver,&ChatMessageReceiver::MessageProcessed,[this]() {
		inactivityClock.start();
	});
	SwapPane(chatPane);

	Command AgendaCommand("agenda","Set the agenda of the stream, displayed in the header of the chat window",CommandType::FORWARD,true);
	chatMessageReceiver->AttachCommand(AgendaCommand);
	Command CommandsCommand("commands","List all of the commands Celeste recognizes",CommandType::FORWARD,false);
	chatMessageReceiver->AttachCommand(CommandsCommand);
	Command PanicCommand("panic","Crash Celeste",CommandType::FORWARD,true);
	chatMessageReceiver->AttachCommand(PanicCommand);
	Command ShoutOutCommand("so","Call attention to another streamer's channel",CommandType::FORWARD,false);
	chatMessageReceiver->AttachCommand(ShoutOutCommand);
	Command SongCommand("song","Show the title, album, and artist of the song that is currently playing",CommandType::FORWARD,false);
	chatMessageReceiver->AttachCommand(SongCommand);
	Command TimezoneCommand("timezone","Display the timezone of the system the bot is running on",CommandType::FORWARD,false);
	chatMessageReceiver->AttachCommand(TimezoneCommand);
	Command UpdateCommand("update","Refresh database of content such as emotes",CommandType::FORWARD,true);
	chatMessageReceiver->AttachCommand(UpdateCommand);
	Command UptimeCommand("uptime","Show how long the bot has been connected",CommandType::FORWARD,false);
	chatMessageReceiver->AttachCommand(UptimeCommand);
	Command VibeCommand("vibe","Start the playlist of music for the stream",CommandType::FORWARD,true);
	chatMessageReceiver->AttachCommand(VibeCommand);
	Command VolumeCommand("volume","Adjust the volume of the vibe keeper",CommandType::FORWARD,true);
	chatMessageReceiver->AttachCommand(VolumeCommand);
	const std::unordered_map<QString,Commands> commands={
		{AgendaCommand.Name(),Commands::AGENDA},
		{CommandsCommand.Name(),Commands::COMMANDS},
		{PanicCommand.Name(),Commands::PANIC},
		{ShoutOutCommand.Name(),Commands::SHOUTOUT},
		{SongCommand.Name(),Commands::SONG},
		{TimezoneCommand.Name(),Commands::TIMEZONE},
		{UpdateCommand.Name(),Commands::UPDATE},
		{UptimeCommand.Name(),Commands::UPTIME},
		{VibeCommand.Name(),Commands::VIBE},
		{VolumeCommand.Name(),Commands::VOLUME}
	};
	connect(chatMessageReceiver,&ChatMessageReceiver::ForwardCommand,[this,chatMessageReceiver,commands](const Command &command) {
		if (commands.find(command.Name()) == commands.end())
		{
			chatPane->Alert(QString(R"(Command "%1" not fully implemented!)").arg(command.Name()));
			return;
		}
		switch (commands.at(command.Name()))
		{
		case Commands::AGENDA:
			chatPane->SetAgenda(command.Message());
			break;
		case Commands::COMMANDS:
		{
			std::vector<std::pair<QString,double>> user;
			std::vector<std::pair<QString,double>> admin;
			for (const std::pair<QString,Command> &command : chatMessageReceiver->Commands())
			{
				if (command.second.Protected())
				{
					admin.push_back({QString("<div style='margin-bottom: 0;'>!%1</div>").arg(command.second.Name()),0.6});
					admin.push_back({QString("<div style='margin-bottom: 0.5em;'>%1</div>").arg(command.second.Description()),0.5});
				}
				else
				{
					user.push_back({QString("<div style='margin-bottom: 0;'>!%1</div>").arg(command.second.Name()),0.6});
					user.push_back({QString("<div style='margin-bottom: 0.5em;'>%1</div>").arg(command.second.Description()),0.5});
				}
			}
			AnnouncePane *pane;
			int duration=15000;
			int count=8; // TODO: this should be calculated based on text line height vs pixel height of window
			count=count*2; // a single command covers two lines
			for (int page=0; page < std::ceil(static_cast<double>(user.size())/count); page++)
			{
				std::vector<std::pair<QString,double>>::size_type start=page*count;
				std::vector<std::pair<QString,double>>::size_type end=start+count;
				pane=new AnnouncePane({&user[start],&user[std::min(user.size(),end)]});
				pane->Duration(duration);
				StageEphemeralPane(pane);
			}
			pane=new AnnouncePane(admin);
			pane->Duration(duration);
			StageEphemeralPane(pane);
			break;
		}
		case Commands::PANIC:
		{
			QFile outputFile(Filesystem::DataPath().absoluteFilePath("panic.txt"));
			QString outputText;
			if (outputFile.open(QIODevice::ReadOnly))
			{
				outputText=outputFile.readAll();
				outputFile.close();
			}
			SwapPane(new StatusPane(this));
			QString date=QDateTime::currentDateTime().toString("ddd d hh:mm:ss");
			outputText=outputText.split("\n").join(QString("\n%1 ").arg(date));
			emit Print(date+"\n"+outputText);
			this->disconnect();
			break;
		}
		case Commands::SHOUTOUT:
		{
			UserRecognizer *user=new UserRecognizer(QString(command.Message()).remove("@"));
			connect(user,&UserRecognizer::Recognized,[this,user]() {
				ImageAnnouncePane *pane=new ImageAnnouncePane({
					{"Drop a follow on<br>",1},
					{QString("%1<br>").arg(user->DisplayName()),1.5},
					{user->Description(),0.5}
					},user->ProfileImage());
				pane->AccentColor(settingAccentColor);
				pane->Duration(10000);
				StageEphemeralPane(pane);
			});
			connect(user,&UserRecognizer::Error,[this](const QString &message) {
				emit Print(Console::GenerateMessage(QCoreApplication::applicationName(),"recognize user",message));
			});
			break;
		}
		case Commands::SONG:
		{
			ImageAnnouncePane *pane=Tuple::New<ImageAnnouncePane,QString,QImage>(CurrentSong());
			pane->AccentColor(settingAccentColor);
			StageEphemeralPane(pane);
			break;
		}
		case Commands::TIMEZONE:
			chatPane->Alert(QDateTime::currentDateTime().timeZone().displayName(QDateTime::currentDateTime().timeZone().isDaylightTime(QDateTime::currentDateTime()) ? QTimeZone::DaylightTime : QTimeZone::StandardTime,QTimeZone::LongName));
			break;
		case Commands::UPDATE:
		{
			if (worker.isRunning())
			{
				chatPane->Alert("Another job is already running.");
				break;
			}
			QNetworkAccessManager *manager=new QNetworkAccessManager(this);
			connect(manager,&QNetworkAccessManager::finished,[this,manager](QNetworkReply *reply) {
				worker=QtConcurrent::run([this,manager,reply]() {
					if (reply->error())
					{
						chatPane->Alert(QString("Network call failed: %1").arg(reply->errorString()));
						return;
					}
					chatPane->Alert("Parsing emote list...");
					QJsonParseError jsonError;
					QJsonDocument json=QJsonDocument::fromJson(reply->readAll(),&jsonError);
					if (json.isNull()) chatPane->Alert(QString("Unregonized reponse from Twitch: %1").arg(jsonError.errorString()));
					chatPane->Alert("Rebuilding data structure...");

					QFile commandListFile(Filesystem::DataPath().filePath(EMOTE_FILENAME));
					if (!commandListFile.open(QIODevice::ReadWrite)) return;
					commandListFile.write("{\n");
					for (const QJsonValue &entry : json.object().value("emoticons").toArray())
					{
						QStringList fragments=entry.toObject().value("images").toObject().value("url").toString().split("/",StringConvert::Split::Behavior(StringConvert::Split::Behaviors::SKIP_EMPTY_PARTS));
						if (fragments.size() < 5) continue;
						commandListFile.write(StringConvert::ByteArray(QString("\t\"%1\": \"%2\",\n").arg(entry.toObject().value("regex").toString(),fragments.at(4))));
					}
					commandListFile.seek(commandListFile.pos()-2);
					commandListFile.write("\n}");
					commandListFile.close();

					manager->deleteLater();
				});
			});
			QNetworkRequest request({TWITCH_API_ENDPOINT_EMOTE_LIST});
			request.setRawHeader("Accept",TWITCH_API_VERSION_5);
			request.setRawHeader("Client-ID",settingClientID);
			Relay::Status::Context *statusContext=chatPane->Alert("Downloading emote list...");
			connect(manager->get(request),&QNetworkReply::downloadProgress,[this,statusContext](qint64 received,qint64 total) {
				statusContext->Updated(QLocale::system().formattedDataSize(received));
				statusContext->ResetClock();
			});
			break;
		}
		case Commands::UPTIME:
		{
			Relay::Status::Context *statusContext=chatPane->Alert(Uptime());
			connect(statusContext,&Relay::Status::Context::UpdateRequested,[this,statusContext]() {
				emit statusContext->Updated(Uptime());
			});
			break;
		}
		case Commands::VIBE:
			if (vibeKeeper->state() == QMediaPlayer::PlayingState)
			{
				chatPane->Alert("Pausing the vibes...");
				vibeKeeper->pause();
				break;
			}
			vibeKeeper->play();
			break;
		case Commands::VOLUME:
			if (command.Message().isEmpty())
			{
				if (vibeFader)
				{
					vibeFader->Abort();
					chatPane->Alert("Aborting volume change...");
				}
				break;
			}
			try
			{
				vibeFader=new Volume::Fader(vibeKeeper,command.Message(),this);
				connect(vibeFader,&Volume::Fader::Feedback,chatPane,&ChatPane::Alert);
				vibeFader->Start();
			}
			catch (const std::range_error &exception)
			{
				chatPane->Alert(QString("%1\n\n%2").arg("Failed to adjust volume!",exception.what()));
			}
			break;
		}
	});

	if (settingVibePlaylist)
	{
		connect(&vibeSources,&QMediaPlaylist::loadFailed,[this]() {
			chatPane->Alert(QString("<b>Failed to load vibe playlist</b><br>%2").arg(vibeSources.errorString()));
		});
		connect(&vibeSources,&QMediaPlaylist::loaded,[this]() {
			vibeSources.shuffle();
			vibeSources.setCurrentIndex(Random::Bounded(0,vibeSources.mediaCount()));
			vibeKeeper->setPlaylist(&vibeSources);
			chatPane->Alert("Vibe playlist loaded!");
		});
		vibeSources.load(QUrl::fromLocalFile(settingVibePlaylist));
		connect(vibeKeeper,QOverload<QMediaPlayer::Error>::of(&QMediaPlayer::error),[this](QMediaPlayer::Error error) {
			chatPane->Alert(QString("<b>Vibe Keeper failed to start</b><br>%2").arg(vibeKeeper->errorString()));
		});
		connect(vibeKeeper,&QMediaPlayer::stateChanged,[this](QMediaPlayer::State state) {
			if (state == QMediaPlayer::PlayingState) chatPane->Alert(std::get<QString>(CurrentSong()));
		});
	}

	if (settingRoasts)
	{
		connect(&roastSources,&QMediaPlaylist::loadFailed,[this]() {
			chatPane->Alert(QString("<b>Failed to load roasts</b><br>%2").arg(roastSources.errorString()));
		});
		connect(&roastSources,&QMediaPlaylist::loaded,[this]() {
			roastSources.shuffle();
			roastSources.setCurrentIndex(Random::Bounded(0,roastSources.mediaCount()));
			roaster->setPlaylist(&roastSources);
			connect(&inactivityClock,&QTimer::timeout,roaster,&QMediaPlayer::play);
			chatPane->Alert("Roasts loaded!");
		});
		roastSources.load(QUrl::fromLocalFile(settingRoasts));
		connect(roaster,QOverload<QMediaPlayer::Error>::of(&QMediaPlayer::error),[this](QMediaPlayer::Error error) {
			chatPane->Alert(QString("<b>Roaster failed to start</b><br>%2").arg(vibeKeeper->errorString()));
		});
		connect(roaster,&QMediaPlayer::mediaStatusChanged,[this](QMediaPlayer::MediaStatus status) {
			if (status == QMediaPlayer::EndOfMedia)
			{
				roaster->stop();
				roastSources.setCurrentIndex(Random::Bounded(0,roastSources.mediaCount()));
			}
		});
	}

	connect(&helpClock,&QTimer::timeout,[this,chatMessageReceiver]() {
		if (!ephemeralPanes.empty()) return;
		try
		{
			const Command &command=chatMessageReceiver->RandomCommand();
			StageEphemeralPane(new AnnouncePane(QString("<h2>!%1</h2><br>%2").arg(
				command.Name(),
				command.Description()
			)));
		}
		catch (const std::range_error &exception)
		{
			chatPane->Alert(QString("Failed to show hint for random command<br>%1").arg(exception.what()));
		}
	});
	helpClock.start();

	connect(&inactivityClock,&QTimer::timeout,[this]() {
		if (settingPortraitVideo) StageEphemeralPane(new VideoPane(settingPortraitVideo));
	});
	inactivityClock.start();
}

void Window::AnnounceArrival(Viewer viewer)
{
	if (settingAdministrator != viewer->Name() && QDateTime::currentDateTime().toMSecsSinceEpoch()-lastRaid.toMSecsSinceEpoch() > static_cast<qint64>(settingRaidInterruptDuration))
	{
		emit RequestEphemeralPane(new MultimediaAnnouncePane({
			{"Please welcome<br>",1},
			{QString("%1<br>").arg(viewer->DisplayName()),1.5},
			{"to the chat",1}
		},viewer->ProfileImage(),ArrivalSound()));
	}
}

/*!
 * \fn Window::StageEphemeralPane
 * \brief Add an ephemeral pane to the queue of panes to be displayed
 *
 * If this is the first ephemeral pane, it will be shown immediately, otherwise
 * it will be shown when the pane before it is finished. The currently visible
 * persistent pane is hidden while the queue contains ephemeral panes.
 *
 * \param pane The pane to be shown
 */
void Window::StageEphemeralPane(EphemeralPane *pane)
{
	QMetaObject::Connection *finishedTrigger=new QMetaObject::Connection;
	*finishedTrigger=connect(pane,&EphemeralPane::Finished,[this,pane,finishedTrigger]() {
		disconnect(*finishedTrigger);
		delete finishedTrigger;
		ReleaseLiveEphemeralPane();
	});
	background->layout()->addWidget(pane);
	if (ephemeralPanes.size() > 0)
	{
		connect(ephemeralPanes.back(),&EphemeralPane::Finished,pane,&EphemeralPane::Show);
		ephemeralPanes.push(pane);
	}
	else
	{
		ephemeralPanes.push(pane);
		visiblePane->hide();
		pane->Show();
	}
}

/*!
 * \fn Window::ReleaseLiveEphemeralPane
 * \brief Removes visible emphemeral pane from the queue
 *
 * When the queue becomes empty, this will instead show the persistent pane that
 * was visible before processing of the queue of ephemeral panes was started.
 */
void Window::ReleaseLiveEphemeralPane()
{
	if (ephemeralPanes.empty()) throw std::logic_error("Ran out of ephemeral panes but messages still coming in to remove them"); // FIXME: this is undefined behavior since it's being thrown from a slot
	ephemeralPanes.pop();
	if (ephemeralPanes.empty()) visiblePane->show();
}

std::tuple<QString,QImage> Window::CurrentSong() const
{
	return {
		QString("Now playing %1 by %2\n\nfrom the ablum %3").arg(
			vibeKeeper->metaData("Title").toString(),
			vibeKeeper->metaData("AlbumArtist").toString(),
			vibeKeeper->metaData("AlbumTitle").toString()
		),
		vibeKeeper->metaData("CoverArtImage").value<QImage>()
	};
}

/*!
 * \fn Window::Uptime
 * \brief Calculates the difference between now and when the bot was started
 * \return A text representation of the difference
 */
const QString Window::Uptime() const
{
	std::chrono::milliseconds duration=TimeConvert::Now()-uptime;
	std::chrono::hours hours=std::chrono::duration_cast<std::chrono::hours>(duration);
	std::chrono::minutes minutes=std::chrono::duration_cast<std::chrono::minutes>(duration-hours);
	std::chrono::seconds seconds=std::chrono::duration_cast<std::chrono::seconds>(duration-minutes);
	return QString("Connection to Twitch has been live for %1 hours, %2 minutes, and %3 seconds").arg(StringConvert::Integer(hours.count()),StringConvert::Integer(minutes.count()),StringConvert::Integer(seconds.count()));
}

/*!
 * \fn Window::ScreenThird
 * \brief Calculates a third of the user's screen size
 * \return A square QSize the size of 1/3 the shortes side of the screen
 */
const QSize Window::ScreenThird()
{
	QSize screenSize=QSize(QGuiApplication::primaryScreen()->geometry().size());
	int shortestSide=std::min(screenSize.width(),screenSize.height())/3;
	return {shortestSide,shortestSide};
}

EventSubscriber* Window::CreateEventSubscriber(const QString &channelOwnerID)
{
	return new EventSubscriber(channelOwnerID,this);
}

const QString Window::ArrivalSound() const
{
	return FileRecognizer(settingArrivalSound).Random();
}
