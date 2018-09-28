/*
 * Copyright © 2017 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranties of
 * MERCHANTABILITY, SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *     Ted Gould <ted.gould@canonical.com>
 */

#include "app-store-base.h"
#include "app-store-legacy.h"
#ifdef HAVE_LIBERTINE
#include "app-store-libertine.h"
#endif
#include "app-store-snap.h"

namespace ubuntu
{
namespace app_launch
{
namespace app_store
{

Base::Base(const std::shared_ptr<Registry::Impl>& registry)
    : info_watcher::Base(registry)
{
}

Base::~Base()
{
}

std::list<std::shared_ptr<Base>> Base::allAppStores(const std::shared_ptr<Registry::Impl>& registry)
{
    return {
        std::make_shared<Legacy>(registry) /* Legacy */
        ,
#ifdef HAVE_LIBERTINE
        std::make_shared<Libertine>(registry) /* Libertine */
        ,
#endif
        std::make_shared<Snap>(registry) /* Snappy */
    };
}

}  // namespace app_store
}  // namespace app_launch
}  // namespace ubuntu
