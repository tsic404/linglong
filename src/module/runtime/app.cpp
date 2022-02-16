/*
 * Copyright (c) 2021. Uniontech Software Ltd. All rights reserved.
 *
 * Author:     Iceyer <me@iceyer.net>
 *
 * Maintainer: Iceyer <me@iceyer.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "app.h"

#include <unistd.h>
#include <sys/wait.h>
#include <linux/prctl.h>
#include <sys/prctl.h>
#include <QProcess>
#include <QFile>
#include <QStandardPaths>
#include <QDir>

#include "module/util/yaml.h"
#include "module/util/uuid.h"
#include "module/util/json.h"
#include "module/util/fs.h"
#include "module/util/xdg.h"
#include "module/util/desktop_entry.h"
#include "module/package/info.h"
#include "module/repo/repo.h"
#include "module/flatpak/flatpak_manager.h"

#define LINGLONG 118

#define LL_VAL(str) #str
#define LL_TOSTRING(str) LL_VAL(str)

using namespace linglong;

class AppPrivate
{
public:
    explicit AppPrivate(App *parent)
        : q_ptr(parent)
    {
    }

    bool init()
    {
        QFile jsonFile(":/config.json");
        if (!jsonFile.open(QIODevice::ReadOnly)) {
            qCritical() << jsonFile.error() << jsonFile.errorString();
            return false;
        }
        auto json = QJsonDocument::fromJson(jsonFile.readAll());
        r = fromVariant<Runtime>(json.toVariant());
        r->setParent(q_ptr);

        container = new Container(q_ptr);
        container->create();

        return true;
    }

    int prepare()
    {
        Q_Q(App);

        // FIXME: get info from module/package
        auto runtimeRef = package::Ref(q->runtime->ref);
        QString runtimeRootPath = repo->rootOfLayer(runtimeRef);

        // FIXME: return error if files not exist
        auto fixRuntimePath = runtimeRootPath + "/files";
        if (!util::dirExists(fixRuntimePath)) {
            fixRuntimePath = runtimeRootPath;
        }

        auto appRef = package::Ref(q->package->ref);
        QString appRootPath = repo->rootOfLayer(appRef);

        stageRootfs(runtimeRef.appId, fixRuntimePath, appRef.appId, appRootPath);

        stageSystem();
        stageHost();
        stageUser(appRef.appId);
        stageMount();

        auto envFilepath = container->workingDirectory + QString("/env");
        QFile envFile(envFilepath);
        if (!envFile.open(QIODevice::WriteOnly)) {
            qCritical() << "create env failed" << envFile.error();
        }
        for (const auto &env : r->process->env) {
            envFile.write(env.toLocal8Bit());
            envFile.write("\n");
        }
        envFile.close();

        Mount &m = *new Mount(r);
        m.type = "bind";
        m.options = QStringList {"rbind"};
        m.source = envFilepath;
        m.destination = "/run/app/env";
        r->mounts.push_back(&m);

        // TODO: move to class package
        // find desktop file
        QDir applicationsDir(QStringList {appRootPath, "entries", "applications"}.join(QDir::separator()));
        auto desktopFilenameList = applicationsDir.entryList({"*.desktop"}, QDir::Files);
        if (useFlatpakRuntime) {
            desktopFilenameList = flatpak::FlatpakManager::instance()->getAppDesktopFileList(appRef.appId);
        }
        if (desktopFilenameList.length() <= 0) {
            return -1;
        }

        util::DesktopEntry desktopEntry(applicationsDir.absoluteFilePath(desktopFilenameList.value(0)));

        if (r->process->args.isEmpty() && !desktopExec.isEmpty()) {
            r->process->args = util::parseExec(desktopExec);
        } else if (r->process->args.isEmpty()) {
            r->process->args = util::parseExec(desktopEntry.rawValue("Exec"));
        }
        // ll-cli run appId 获取的是原生desktop exec ,有的包含%F等参数，需要去掉
        // FIXME(liujianqiang):后续整改，参考下面链接
        // https://github.com/linuxdeepin/go-lib/blob/28a4ee3e8dbe6d6316d3b0053ee4bda1a7f63f98/appinfo/desktopappinfo/desktopappinfo.go
        // https://github.com/linuxdeepin/go-lib/commit/bd52a27688413e1273f8b516ef55dc472d7978fd
        auto indexNum = r->process->args.indexOf(QRegExp("^%\\w$"));
        if (indexNum != -1) {
            r->process->args.removeAt(indexNum);
        }

        qDebug() << "exec" << r->process->args;
        return 0;
    }

    int stageSystem() const
    {
        QList<QPair<QString, QString>> mountMap;
        mountMap = {
            {"/dev/dri", "/dev/dri"},
            {"/dev/snd", "/dev/snd"},
        };

        for (const auto &pair : mountMap) {
            Mount &m = *new Mount(r);
            m.type = "bind";
            m.options = QStringList {"rbind"};
            m.source = pair.first;
            m.destination = pair.second;
            r->mounts.push_back(&m);
            qDebug() << "mount stageSystem" << m.source << m.destination;
        }
        return 0;
    }

    int stageRootfs(const QString &runtimeId, QString runtimeRootPath, const QString &appId, QString appRootPath) const
    {
        bool useThinRuntime = true;
        bool fuseMount = false;

        // if use wine runtime, mount with fuse
        // FIXME(iceyer): use info.json to decide use fuse or not
        if (runtimeRootPath.contains("org.deepin.Wine")) {
            fuseMount = true;
        }

        if (useFlatpakRuntime) {
            fuseMount = false;
            useThinRuntime = false;
        }

        r->annotations = new Annotations(r);
        r->annotations->container_root_path = container->workingDirectory;

        if (fuseMount) {
            r->annotations->overlayfs = new AnnotationsOverlayfsRootfs(r->annotations);
            r->annotations->overlayfs->lower_parent =
                QStringList {container->workingDirectory, ".overlayfs", "lower_parent"}.join("/");
            r->annotations->overlayfs->upper =
                QStringList {container->workingDirectory, ".overlayfs", "upper"}.join("/");
            r->annotations->overlayfs->workdir =
                QStringList {container->workingDirectory, ".overlayfs", "workdir"}.join("/");
        } else {
            r->annotations->native = new AnnotationsNativeRootfs(r->annotations);
        }

        QList<QPair<QString, QString>> mountMap;

        if (useThinRuntime) {
            mountMap = {
                {"/usr", "/usr"},
                {"/etc", "/etc"},
                {runtimeRootPath + "/usr", "/runtime"},
            };

            // FIXME(iceyer): extract for wine, remove later
            if (fuseMount) {
                // NOTE: the override should be before host /usr, MUST in the front of all
                mountMap.push_front({runtimeRootPath + "/usr", "/usr"});
                mountMap.push_back({runtimeRootPath + "/opt/deepinwine", "/opt/deepinwine"});
                mountMap.push_back({runtimeRootPath + "/opt/deepin-wine6-stable", "/opt/deepin-wine6-stable"});
            }
        } else {
            if (useFlatpakRuntime) {
                runtimeRootPath = flatpak::FlatpakManager::instance()->getRuntimePath(appId);
            }
            // FIXME(iceyer): if runtime is empty, use the last
            if (runtimeRootPath.isEmpty()) {
                qCritical() << "mount runtime failed" << runtimeRootPath;
                return -1;
            }

            mountMap.push_back({runtimeRootPath, "/usr"});
        }

        if (useFlatpakRuntime) {
            appRootPath = flatpak::FlatpakManager::instance()->getAppPath(appId);
            mountMap.push_back({appRootPath, "/app"});
        } else {
            mountMap.push_back({appRootPath, "/opt/apps/" + appId});
            // TODO(iceyer): add doc for this or remove
            mountMap.push_back({QStringList {appRootPath, "files/lib"}.join("/"), "/run/app/lib"});
        }

        for (const auto &pair : mountMap) {
            auto m = new Mount(r);
            m->type = "bind";
            m->options = QStringList {"ro", "rbind"};
            m->source = pair.first;
            m->destination = pair.second;

            if (fuseMount) {
                r->annotations->overlayfs->mounts.push_back(m);
            } else {
                r->annotations->native->mounts.push_back(m);
            }
        }

        // TODO(iceyer): let application do this or add to doc
        auto appLdLibraryPath = QStringList {"/opt/apps", appId, "files/lib"}.join("/");
        if (useFlatpakRuntime) {
            appLdLibraryPath = "/app/lib";
        }

        // TODO(iceyer): support other arch, or just no arch?
        auto fixLdLibraryPath = QStringList {
            appLdLibraryPath,
            "/runtime/lib",
            "/runtime/lib/x86_64-linux-gnu",
            "/runtime/lib/i386-linux-gnu",
        };
        r->process->env.push_back("LD_LIBRARY_PATH=" + fixLdLibraryPath.join(":"));
        r->process->env.push_back("QT_PLUGIN_PATH=/usr/lib/plugins:/runtime/plugins");
        r->process->env.push_back("QT_QPA_PLATFORM_PLUGIN_PATH=/usr/lib/plugins/platforms:/runtime/plugins/platforms");
        return 0;
    }

    int stageHost() const
    {
        QList<QPair<QString, QString>> roMountMap = {
            {"/etc/resolv.conf", "/run/host/network/etc/resolv.conf"},
            {"/run/resolvconf", "/run/resolvconf"},
            {"/usr/share/fonts", "/run/host/appearance/fonts"},
            {"/usr/share/locale/", "/usr/share/locale/"},
            {"/usr/lib/locale/", "/usr/lib/locale/"},
            {"/usr/share/fonts", "/usr/share/fonts"},
            {"/usr/share/themes", "/usr/share/themes"},
            {"/usr/share/icons", "/usr/share/icons"},
            {"/usr/share/zoneinfo", "/usr/share/zoneinfo"},
            {"/etc/localtime", "/run/host/etc/localtime"},
            {"/etc/machine-id", "/run/host/etc/machine-id"},
            {"/etc/machine-id", "/etc/machine-id"},
            {"/var", "/var"}, // FIXME: should we mount /var as "ro"?
            {"/var/cache/fontconfig", "/run/host/appearance/fonts-cache"},
        };

        for (auto const &item : QDir("/dev").entryInfoList({"nvidia*"}, QDir::AllEntries | QDir::System)) {
            roMountMap.push_back({item.canonicalFilePath(), item.canonicalFilePath()});
        }

        for (const auto &pair : roMountMap) {
            Mount &m = *new Mount(r);
            m.type = "bind";
            m.options = QStringList {"ro", "rbind"};
            m.source = pair.first;
            m.destination = pair.second;
            r->mounts.push_back(&m);
            qDebug() << "mount app" << m.source << m.destination;
        }

        QList<QPair<QString, QString>> mountMap = {
            {"/tmp/.X11-unix", "/tmp/.X11-unix"}, // FIXME: only mount one DISPLAY
        };

        for (const auto &pair : mountMap) {
            Mount &m = *new Mount(r);
            m.type = "bind";
            m.options = QStringList {"rbind"};
            m.source = pair.first;
            m.destination = pair.second;
            r->mounts.push_back(&m);
            qDebug() << "mount app" << m.source << m.destination;
        }

        return 0;
    }

    int stageUser(const QString &appId) const
    {
        QList<QPair<QString, QString>> mountMap;

        // bind user data
        auto userRuntimeDir = QString("/run/user/%1").arg(getuid());
        {
            Mount &m = *new Mount(r);
            m.type = "tmpfs";
            m.options = QStringList {"nodev", "nosuid"};
            m.source = "tmpfs";
            m.destination = userRuntimeDir;
            r->mounts.push_back(&m);
            qDebug() << "mount app" << m.source << m.destination;
        }

        // FIXME: use proxy dbus
        bool useDBusProxy = false;
        if (useDBusProxy) {
            // bind dbus-proxy-user, now use session bus
            mountMap.push_back(qMakePair(userRuntimeDir + "/user-bus", userRuntimeDir + "/bus"));
            // bind dbus-proxy
            mountMap.push_back(qMakePair(userRuntimeDir + "/system-bus", QString("/run/dbus/system_bus_socket")));
        } else {
            mountMap.push_back(qMakePair(userRuntimeDir + "/bus", userRuntimeDir + "/bus"));
            mountMap.push_back(
                qMakePair(QString("/run/dbus/system_bus_socket"), QString("/run/dbus/system_bus_socket")));
        }

        // bind /run/usr/$(uid)/pulse
        mountMap.push_back(qMakePair(userRuntimeDir + "/pulse", userRuntimeDir + "/pulse"));

        auto hostAppHome = util::ensureUserDir({".linglong", appId, "home"});
        mountMap.push_back(qMakePair(hostAppHome, util::getUserFile("")));

        // bind $(HOME)/.linglong/$(appId)
        auto appLinglongPath = util::ensureUserDir({".linglong", appId});
        mountMap.push_back(qMakePair(appLinglongPath, util::getUserFile(".linglong/" + appId)));

        auto appConfigPath = util::ensureUserDir({".linglong", appId, "/config"});
        mountMap.push_back(qMakePair(appConfigPath, util::getUserFile(".config")));

        auto appCachePath = util::ensureUserDir({".linglong", appId, "/cache"});
        mountMap.push_back(qMakePair(appCachePath, util::getUserFile(".cache")));

        mountMap.push_back(qMakePair(userRuntimeDir + "/dconf", userRuntimeDir + "/dconf"));

        mountMap.push_back(
            qMakePair(util::getUserFile(".config/user-dirs.dirs"), util::getUserFile(".config/user-dirs.dirs")));

        for (const auto &pair : mountMap) {
            Mount &m = *new Mount(r);
            m.type = "bind";
            m.options = QStringList {"rbind"};

            m.source = pair.first;
            m.destination = pair.second;
            r->mounts.push_back(&m);
            qDebug() << "mount app" << m.source << m.destination;
        }

        QList<QPair<QString, QString>> roMountMap;
        roMountMap.push_back(
            qMakePair(util::getUserFile(".local/share/fonts"), util::getUserFile(".local/share/fonts")));

        roMountMap.push_back(
            qMakePair(util::getUserFile(".config/fontconfig"), util::getUserFile(".config/fontconfig")));

        // mount fonts
        roMountMap.push_back(
            qMakePair(util::getUserFile(".local/share/fonts"), QString("/run/host/appearance/user-fonts")));

        // mount fonts cache
        roMountMap.push_back(
            qMakePair(util::getUserFile(".cache/fontconfig"), QString("/run/host/appearance/user-fonts-cache")));

        QString xauthority = getenv("XAUTHORITY");
        roMountMap.push_back(qMakePair(xauthority, xauthority));

        for (const auto &pair : roMountMap) {
            Mount &m = *new Mount(r);
            m.type = "bind";
            m.options = QStringList {"ro", "rbind"};
            m.source = pair.first;
            m.destination = pair.second;
            r->mounts.push_back(&m);
            qDebug() << "mount app" << m.source << m.destination;
        }
        auto appRef = package::Ref(q_ptr->package->ref);
        auto appBinaryPath = QStringList {"/opt/apps", appRef.appId, "files/bin"}.join("/");
        if (useFlatpakRuntime) {
            appBinaryPath = "/app/bin";
        }
        r->process->env.push_back("PATH=" + appBinaryPath + ":" + getenv("PATH"));
        r->process->env.push_back("HOME=" + util::getUserFile(""));
        r->process->env.push_back("XDG_RUNTIME_DIR=" + userRuntimeDir);
        r->process->env.push_back("DBUS_SESSION_BUS_ADDRESS=unix:path=" + util::jonsPath({userRuntimeDir, "bus"}));

        auto appSharePath = QStringList {"/opt/apps", appRef.appId, "files/share"}.join("/");
        if (useFlatpakRuntime) {
            appSharePath = "/app/share";
        }
        auto xdgDataDirs = QStringList {appSharePath, "/runtime/share"};
        xdgDataDirs.append(qEnvironmentVariable("XDG_DATA_DIRS", "/usr/local/share:/usr/share"));
        r->process->env.push_back("XDG_DATA_DIRS=" + xdgDataDirs.join(":"));

        // set env XDG_DATA_HOME=$(HOME)/.linglong/$(appId)/share
        r->process->env.push_back("XDG_DATA_HOME=" + util::getUserFile(".linglong/" + appId + "/share"));

        auto bypassENV = [&](const char *constEnv) {
            r->process->env.push_back(QString(constEnv) + "=" + getenv(constEnv));
        };

        QStringList envList = {"DISPLAY",
                               "LANG",
                               "LANGUAGE",
                               "XAUTHORITY",
                               "XDG_SESSION_DESKTOP",
                               "D_DISABLE_RT_SCREEN_SCALE",
                               "XMODIFIERS",
                               "DESKTOP_SESSION",
                               "DEEPIN_WINE_SCALE",
                               "XDG_CURRENT_DESKTOP",
                               "XIM",
                               "XDG_SESSION_TYPE",
                               "CLUTTER_IM_MODULE",
                               "QT4_IM_MODULE",
                               "GTK_IM_MODULE"};

        for (auto &env : envList) {
            bypassENV(env.toStdString().c_str());
        }
        qDebug() << r->process->env;
        r->process->cwd = util::getUserFile("");

        QList<QList<quint64>> uidMaps = {
            {getuid(), 0, 1},
        };
        for (auto const &uidMap : uidMaps) {
            Q_ASSERT(uidMap.size() == 3);
            auto idMap = new IdMap(r->linux);
            idMap->hostId = uidMap.value(0);
            idMap->containerId = uidMap.value(1);
            idMap->size = uidMap.value(2);
            r->linux->uidMappings.push_back(idMap);
        }

        QList<QList<quint64>> gidMaps = {
            {getgid(), 0, 1},
        };
        for (auto const &gidMap : gidMaps) {
            Q_ASSERT(gidMap.size() == 3);
            auto idMap = new IdMap(r->linux);
            idMap->hostId = gidMap.value(0);
            idMap->containerId = gidMap.value(1);
            idMap->size = gidMap.value(2);
            r->linux->gidMappings.push_back(idMap);
        }

        return 0;
    }

    int stageMount()
    {
        Q_Q(const App);

        if (!q->permission) {
            return 0;
        }

        QMap<QString, std::function<QString()>> replacementMap = {
            {"${HOME}", []() -> QString { return util::getUserFile(""); }},
        };

        auto pathPreprocess = [&](QString path) -> QString {
            auto keys = replacementMap.keys();
            for (const auto &key : keys) {
                path.replace(key, (replacementMap.value(key))());
            }
            return path;
        };

        //    TODO: debug mount for developer
        for (const auto &mount : q->permission->mounts) {
            Mount &m = *new Mount(r);
            m.type = "bind";
            m.options = QStringList {"rbind"};
            auto component = mount.split(":");
            m.source = pathPreprocess(component.value(0));
            m.destination = pathPreprocess(component.value(1));
            r->mounts.push_back(&m);
            qDebug() << "mount app" << m.source << m.destination;
        }

        return 0;
    }

    // FIXME: none static
    static QString loadConfig(linglong::repo::Repo *repo, const QString &appId, const QString &appVersion,
                              bool isFlatpakApp = false)
    {
        util::ensureUserDir({".linglong", appId});

        auto configPath = getUserFile(QString("%1/%2/app.yaml").arg(".linglong", appId));

        // create yaml form info
        // auto appRoot = LocalRepo::get()->rootOfLatest();
        auto latestAppRef = repo->latestOfRef(appId, appVersion);

        auto appInstallRoot = repo->rootOfLayer(latestAppRef);

        auto appInfo = appInstallRoot + "/info.json";
        // 判断是否存在
        if (!isFlatpakApp && !fileExists(appInfo)) {
            qCritical() << appInfo << " not exist";
            return "";
        }

        // create a yaml config from json
        auto info = util::loadJSON<package::Info>(appInfo);

        if (info->runtime.isEmpty()) {
            // FIXME: return error is not exist

            // thin runtime
            info->runtime = "org.deepin.Runtime/20/x86_64";

            // full runtime
            // info->runtime = "deepin.Runtime.Sdk/23/x86_64";
        }

        package::Ref runtimeRef(info->runtime);

        QMap<QString, QString> variables = {
            {"APP_REF", latestAppRef.toLocalRefString()},
            {"RUNTIME_REF", runtimeRef.toLocalRefString()},
        };

        // TODO: remove to util module as file_template.cpp

        QFile templateFile(":/app.yaml");
        templateFile.open(QIODevice::ReadOnly);
        auto templateData = templateFile.readAll();
        foreach (auto const &k, variables.keys()) {
            templateData.replace(QString("@%1@").arg(k).toLocal8Bit(), variables.value(k).toLocal8Bit());
        }
        templateFile.close();

        QFile configFile(configPath);
        configFile.open(QIODevice::WriteOnly | QIODevice::Truncate);
        configFile.write(templateData);
        configFile.close();

        return configPath;
    }

    bool useFlatpakRuntime = false;
    QString desktopExec = nullptr;

    Container *container = nullptr;
    Runtime *r = nullptr;
    App *q_ptr = nullptr;

    repo::Repo *repo;

    Q_DECLARE_PUBLIC(App);
};

App::App(QObject *parent)
    : JsonSerialize(parent)
    , dd_ptr(new AppPrivate(this))
{
}

App *App::load(linglong::repo::Repo *repo, const package::Ref &ref, const QString &desktopExec, bool useFlatpakRuntime)
{
    QString configPath = AppPrivate::loadConfig(repo, ref.appId, ref.version, useFlatpakRuntime);
    if (!fileExists(configPath)) {
        return nullptr;
    }

    QFile appConfig(configPath);
    appConfig.open(QIODevice::ReadOnly);

    qDebug() << "load conf yaml from" << configPath;

    App *app = nullptr;
    try {
        auto data = QString::fromLocal8Bit(appConfig.readAll());
        qDebug() << data;
        YAML::Node doc = YAML::Load(data.toStdString());
        app = formYaml<App>(doc);

        qDebug() << app << app->runtime << app->package << app->version;
        // TODO: maybe set as an arg of init is better
        app->dd_ptr->useFlatpakRuntime = useFlatpakRuntime;
        app->dd_ptr->desktopExec = desktopExec;
        app->dd_ptr->repo = repo;
        app->dd_ptr->init();
    } catch (...) {
        qCritical() << "FIXME: load config failed, use default app config";
    }
    return app;
}

int App::start()
{
    Q_D(App);

    d->r->root->path = d->container->workingDirectory + "/root";
    util::ensureDir(d->r->root->path);

    d->prepare();

    // write pid file
    QFile pidFile(d->container->workingDirectory + QString("/%1.pid").arg(getpid()));
    pidFile.open(QIODevice::WriteOnly);
    pidFile.close();

    qDebug() << "start container at" << d->r->root->path;
    auto json = QJsonDocument::fromVariant(toVariant<Runtime>(d->r)).toJson();
    auto data = json.toStdString();

    int pipeEnds[2];
    if (pipe(pipeEnds) != 0) {
        return EXIT_FAILURE;
    }

    prctl(PR_SET_PDEATHSIG, SIGKILL);

    pid_t boxPid = fork();
    if (boxPid < 0) {
        return -1;
    }

    if (0 == boxPid) {
        // child process
        (void)close(pipeEnds[1]);
        if (dup2(pipeEnds[0], LINGLONG) == -1) {
            return EXIT_FAILURE;
        }
        (void)close(pipeEnds[0]);

        char const *const args[] = {"/usr/bin/ll-box", LL_TOSTRING(LINGLONG), NULL};
        int ret = execvp(args[0], (char **)args);
        exit(ret);
    } else {
        close(pipeEnds[0]);
        write(pipeEnds[1], data.c_str(), data.size());
        close(pipeEnds[1]);

        d->container->pid = boxPid;
        // FIXME(interactive bash): if need keep interactive shell
        waitpid(boxPid, nullptr, 0);
    }

    return EXIT_SUCCESS;
}

Container *App::container() const
{
    Q_D(const App);
    return d->container;
}

App::~App() = default;
