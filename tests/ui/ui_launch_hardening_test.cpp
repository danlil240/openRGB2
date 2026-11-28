/* ui_launch_hardening_test — LaunchHardening policy:
   - the QA socket is refused for an elevated or managed UI
     (whether QA came from --qa or OPENRGB2_QA);
   - PowerShell children get a sanitised environment (no CLR
     profiler hooks, system-only PSModulePath, no Qt search
     vars), checked both on the map and in a real child;
   - the ownership-conflict contract parses from captured
     output (exit 3 banner + "ownership CONFLICT:" lines). */

#include "LaunchHardening.h"
#include "OwnershipNotice.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QProcess>

#include <cstdio>

using namespace openrgb2::ui::launch;

static int g_fail = 0;
#define CHECK(cond, what)                                           \
    do {                                                            \
        if(!(cond)) { std::printf("FAIL %s\n", what); ++g_fail; }   \
        else std::printf("ok   %s\n", what);                        \
    } while(0)

static void TestQaPolicy()
{
    CHECK(DecideQa(false, false, false) == QaDecision::NotRequested,
          "qa: not requested");
    CHECK(DecideQa(false, true, true) == QaDecision::NotRequested,
          "qa: not requested even when managed+elevated");
    CHECK(DecideQa(true, false, false) == QaDecision::Allowed,
          "qa: allowed for non-elevated unmanaged");
    CHECK(DecideQa(true, false, true) == QaDecision::RefusedElevated,
          "qa: refused when elevated");
    CHECK(DecideQa(true, true, true) == QaDecision::RefusedElevated,
          "qa: refused for elevated managed (sign-in task)");
    CHECK(DecideQa(true, true, false) == QaDecision::RefusedManaged,
          "qa: refused for explicit --managed");
    CHECK(!QaRefusalMessage(QaDecision::RefusedElevated).isEmpty()
              && !QaRefusalMessage(QaDecision::RefusedManaged).isEmpty()
              && QaRefusalMessage(QaDecision::Allowed).isEmpty(),
          "qa: refusal messages");
}

static void TestEnvMap()
{
    QProcessEnvironment base;
    base.insert("COR_ENABLE_PROFILING", "1");
    base.insert("COR_PROFILER", "{deadbeef}");
    base.insert("COR_PROFILER_PATH", "C:\\evil.dll");
    base.insert("CORECLR_ENABLE_PROFILING", "1");
    base.insert("CORECLR_PROFILER", "{deadbeef}");
    base.insert("CORECLR_PROFILER_PATH", "C:\\evil.dll");
    base.insert("PSModulePath", "C:\\Users\\x\\evil");
    base.insert("QT_PLUGIN_PATH", "C:\\evil");
    base.insert("QML2_IMPORT_PATH", "C:\\evil");
    base.insert("KEEP_ME", "yes");
    const QProcessEnvironment env = SanitizedChildEnvironment(base);
    const char* const gone[] = {
        "COR_ENABLE_PROFILING", "COR_PROFILER", "COR_PROFILER_PATH",
        "CORECLR_ENABLE_PROFILING", "CORECLR_PROFILER",
        "CORECLR_PROFILER_PATH", "QT_PLUGIN_PATH", "QML2_IMPORT_PATH"};
    bool all_gone = true;
    for(const char* g : gone)
        all_gone = all_gone && !env.contains(QString::fromLatin1(g));
    CHECK(all_gone, "env: profiler + Qt search vars removed");
    CHECK(env.value("KEEP_ME") == "yes", "env: unrelated vars kept");
#ifdef Q_OS_WIN
    CHECK(env.value("PSModulePath") == SystemPsModulePath()
              && !env.value("PSModulePath").contains("evil"),
          "env: PSModulePath reset to system module dirs");
    CHECK(QFileInfo(SystemPowerShellPath()).isAbsolute()
              && QFileInfo::exists(SystemPowerShellPath()),
          "env: absolute system powershell.exe");
#endif
}

#ifdef Q_OS_WIN
static void TestRealChild()
{
    qputenv("COR_PROFILER", "{deadbeef}");
    qputenv("COR_ENABLE_PROFILING", "0"); /* never actually load one */
    qputenv("PSModulePath", "C:\\definitely\\not\\system");
    QProcess p;
    PrepareHiddenPowerShell(p);
    p.setArguments({"-NoProfile", "-NonInteractive", "-Command",
        "Write-Output (\"P=\" + $env:COR_PROFILER + \"|E=\" + "
        "$env:COR_ENABLE_PROFILING + \"|M=\" + $env:PSModulePath)"});
    p.start();
    const bool done = p.waitForFinished(30000);
    const QString out = QString::fromLocal8Bit(p.readAllStandardOutput());
    CHECK(done && p.exitCode() == 0, "child: powershell ran");
    CHECK(out.contains("P=|E=|"), "child: profiler vars absent");
    CHECK(!out.contains("definitely"), "child: user PSModulePath dropped");
    qunsetenv("COR_PROFILER");
    qunsetenv("COR_ENABLE_PROFILING");
    qunsetenv("PSModulePath");
}
#endif

static void TestOwnershipParse()
{
    const QString refused = QStringLiteral(
        "==> OpenRGB2 managed startup (C:\\x)\r\n"
        "  ownership CONFLICT: SignalRGB (pid 42, running) -- close it\r\n"
        "  ownership ok: something else\r\n"
        "  ownership CONFLICT: iCUE (pid 7, service) -- close it\r\n"
        "\r\n"
        "OWNERSHIP CONFLICT -- OpenRGB2 will not take over silently.\r\n");
    QStringList c;
    CHECK(ParseOwnershipConflict(refused, &c) && c.size() == 2
              && c[0].startsWith("ownership CONFLICT: SignalRGB")
              && c[1].endsWith("close it"),
          "parse: banner + two competitor lines (CRLF trimmed)");
    QStringList none{"stale"};
    CHECK(!ParseOwnershipConflict("OpenRGB2 started.\n", &none)
              && none.size() == 1,
          "parse: no banner -> false, list untouched");
    CHECK(!ParseOwnershipConflict(QString(), nullptr),
          "parse: empty capture (e.g. log truncated) -> false");
}

/* Audit U-6: engine-reported folders reach the shell only as an
   existing absolute local directory, opened by an absolute
   explorer.exe. */
/* R-10: the tray notice fires once per writer key per session —
   a writer that stays, or comes back, never re-notifies. */
static void TestConflictNotice()
{
    using openrgb2::ui::ConflictNoticeTracker;
    auto conflict = [](const char* owner, const char* key)
    {
        QVariantMap c;
        c.insert(QStringLiteral("owner"), QString::fromUtf8(owner));
        c.insert(QStringLiteral("key"), QString::fromUtf8(key));
        return QVariant(c);
    };
    auto report = [](const QVariantList& conflicts)
    {
        QVariantMap m;
        m.insert(QStringLiteral("conflicts"), conflicts);
        return m;
    };
    ConflictNoticeTracker t;
    CHECK(t.NewConflicts(QVariantMap()).isEmpty(),
          "notice: no report -> nothing");
    QStringList n = t.NewConflicts(report({
        conflict("Windows Dynamic Lighting", "dynamic_lighting") }));
    CHECK(n == QStringList{QStringLiteral("Windows Dynamic Lighting")},
          "notice: first sighting of a key -> one notice");
    CHECK(t.NewConflicts(report({
              conflict("Windows Dynamic Lighting",
                       "dynamic_lighting") })).isEmpty(),
          "notice: same key again -> nothing");
    n = t.NewConflicts(report({
        conflict("Windows Dynamic Lighting", "dynamic_lighting"),
        conflict("Logitech G Hub / Options", "logitech"),
        conflict("Logitech G Hub / Options", "logitech") }));
    CHECK(n == QStringList{QStringLiteral("Logitech G Hub / Options")},
          "notice: only the new key, once even with two processes");
    t.NewConflicts(report({}));
    CHECK(t.NewConflicts(report({
              conflict("Logitech G Hub / Options", "logitech") }))
              .isEmpty(),
          "notice: a key that cleared and came back -> nothing");
    n = t.NewConflicts(report({ conflict("Mystery RGB", "") }));
    CHECK(n == QStringList{QStringLiteral("Mystery RGB")},
          "notice: a keyless finding is keyed by its label");
}

static void TestShellFolder()
{
#ifdef Q_OS_WIN
    QTemporaryDir tmp;
    CHECK(tmp.isValid(), "shell: temp dir");
    const QString dir = tmp.path();                 /* forward slashes */
    QString native, why;
    CHECK(ValidateShellFolder(dir, &native, &why)
              && native == QDir::toNativeSeparators(QDir::cleanPath(dir))
              && why.isEmpty(),
          "shell: existing absolute dir accepted, native separators");
    CHECK(ValidateShellFolder(QDir::toNativeSeparators(dir), &native,
                              &why),
          "shell: backslash form accepted");

    QFile f(dir + QStringLiteral("/file.txt"));
    CHECK(f.open(QIODevice::WriteOnly), "shell: fixture file");
    f.close();

    const QStringList refused = {
        QString(),
        QStringLiteral("relative\\dir"),
        QStringLiteral("C:"),
        QStringLiteral("C:relative"),
        QStringLiteral("\\\\server\\share"),
        QStringLiteral("//server/share"),
        QStringLiteral("\\\\?\\C:\\Windows"),
        QStringLiteral("\\\\.\\C:\\Windows"),
        QStringLiteral("file:///C:/Windows"),
        QStringLiteral("https://example.invalid/x"),
        QStringLiteral("ms-settings:privacy"),
        dir + QStringLiteral("/file.txt"),          /* a file       */
        dir + QStringLiteral("/missing"),           /* not there    */
        dir + QStringLiteral(":stream"),            /* ADS          */
        dir + QStringLiteral("/a\"b"),
        dir + QStringLiteral("/*"),
    };
    for(const QString& r : refused)
    {
        QString n = QStringLiteral("untouched"), w;
        const bool ok = ValidateShellFolder(r, &n, &w);
        const QByteArray label =
            ("shell: refused '" + r + "'").toUtf8();
        CHECK(!ok && !w.isEmpty(), label.constData());
    }

    const QString explorer = SystemExplorerPath();
    CHECK(QFileInfo(explorer).isAbsolute()
              && QFileInfo::exists(explorer)
              && QFileInfo(explorer).fileName().compare(
                     QStringLiteral("explorer.exe"),
                     Qt::CaseInsensitive) == 0,
          "shell: absolute system explorer.exe");
#endif
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    TestQaPolicy();
    TestEnvMap();
#ifdef Q_OS_WIN
    TestRealChild();
#endif
    TestOwnershipParse();
    TestConflictNotice();
    TestShellFolder();
    if(g_fail)
    {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::puts("PASS launch hardening");
    return 0;
}
