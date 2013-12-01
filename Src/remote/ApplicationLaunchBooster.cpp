/* @@@LICENSE
*
* (c) 2013 Simon Busch <morphis@gravedo.de>
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
* LICENSE@@@ */

#include <QQuickView>
#include <QtQml>
#include <glib.h>

#include "ApplicationLaunchBooster.h"

#define QML_PRELOAD_FILE     "/usr/palm/sysmgr/preload.qml"

ApplicationLaunchBooster* ApplicationLaunchBooster::instance()
{
    static ApplicationLaunchBooster *instance = 0;
    if (!instance)
        instance = new ApplicationLaunchBooster;
    return instance;
}

ApplicationLaunchBooster::ApplicationLaunchBooster() :
    m_engine(new QQmlEngine)
{
    preload();
}

void ApplicationLaunchBooster::preload()
{
    QQmlComponent component(m_engine, QUrl::fromLocalFile(QML_PRELOAD_FILE));
    if (!component.isReady()) {
        g_warning("Failed to preload Qt5 quick components:");
        Q_FOREACH(const QQmlError &error, component.errors()) {
            g_warning("Error: %s", error.toString().toUtf8().constData());
        }
        return;
    }

    QQmlContext context(m_engine);
    QObject *obj = component.create(&context);
    if (!obj)
        g_warning("Faild to create preload object");
    delete obj;
}
