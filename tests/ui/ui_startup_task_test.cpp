#include "StartupTaskController.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>

static bool WaitForIdle(openrgb2::ui::StartupTaskController& task)
{
    QElapsedTimer timer;
    timer.start();
    while(task.busy() && timer.elapsed() < 10000)
    {
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
    QCoreApplication::processEvents();
    return !task.busy();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    if(!dir.isValid()) return 1;
    QDir().mkpath(dir.filePath(QStringLiteral("scripts")));
    QFile script(dir.filePath(QStringLiteral("scripts/startup-task.ps1")));
    if(!script.open(QIODevice::WriteOnly)) return 1;
    script.write(R"PS(param([string]$Mode,[string]$InstallDir)
$marker = Join-Path $InstallDir 'enabled.marker'
if($Mode -eq 'enable') { Set-Content -LiteralPath $marker -Value 'on' }
if($Mode -eq 'disable') { Remove-Item -LiteralPath $marker -ErrorAction SilentlyContinue }
[pscustomobject]@{enabled=[bool](Test-Path -LiteralPath $marker)} | ConvertTo-Json -Compress
)PS");
    script.close();

    openrgb2::ui::StartupTaskController task(dir.path());
    task.refresh();
    if(!WaitForIdle(task) || task.enabled() || !task.error().isEmpty())
    {
        std::puts("FAIL initial status"); return 1;
    }
    task.setEnabled(true);
    if(!WaitForIdle(task) || !task.enabled() || !task.error().isEmpty())
    {
        std::puts("FAIL enable"); return 1;
    }
    task.setEnabled(false);
    if(!WaitForIdle(task) || task.enabled() || !task.error().isEmpty())
    {
        std::puts("FAIL disable"); return 1;
    }
    std::puts("PASS startup task controller status/enable/disable");
    return 0;
}
