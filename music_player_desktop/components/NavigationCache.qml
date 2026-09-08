pragma ComponentBehavior: Bound
import QtQuick

Item {
    id: cache
    property string route: "library/songs"
    property var pages: []
    property var visited: ({})
    property var recentRoutes: []
    property var pageStates: ({})
    property var artworkStates: ({})
    property Item transitionSourceItem: null
    property var settingsStore: typeof backendSettingsController !== "undefined" ? backendSettingsController : null
    readonly property bool remember: {
        if (!settingsStore) return true
        const revision = settingsStore.revision
        return settingsStore.value("list.rememberScrollPosition", true)
    }
    onRememberChanged: { if (!remember) { pageStates = ({}); artworkStates = ({}) }; retain() }
    onTransitionSourceItemChanged: retain()
    function isInside(page, source) {
        if (!page || !source) return false
        for (let item = source; item; item = item.parent) if (item === page) return true
        return false
    }
    function artworkFor(page) {
        const requests = [], seen = ({})
        function visit(item) {
            if (item.objectName === "coverSourceImage" && item.cache
                    && (item.status === Image.Loading || (item.status === Image.Ready
                        && (item.implicitWidth > 1 || item.implicitHeight > 1)))) {
                const source = String(item.source), size = item.sourceSize
                const key = source + "|" + size.width + "x" + size.height
                if (!seen[key]) { seen[key] = true; requests.push({ source: source, width: size.width, height: size.height }) }
            }
            for (const child of item.children) visit(child)
        }
        visit(page)
        return requests
    }
    function retainArtwork(artwork) {
        const wanted = ({}), existing = ({})
        for (const key of Object.keys(artwork)) for (const request of artwork[key])
            wanted[request.source + "|" + request.width + "x" + request.height] = request
        for (let i = 0; i < retainedArtwork.count; ++i)
            existing[retainedArtwork.get(i).cacheKey] = true
        // Append before releasing owners, and keep unchanged Image instances.
        // Replacing a JS-array model would drop and reacquire every pixmap.
        for (const key of Object.keys(wanted)) if (!existing[key]) {
            const request = wanted[key]
            retainedArtwork.append({ cacheKey: key, artworkSource: request.source,
                pixelWidth: request.width, pixelHeight: request.height })
        }
        for (let i = retainedArtwork.count - 1; i >= 0; --i)
            if (!wanted[retainedArtwork.get(i).cacheKey]) retainedArtwork.remove(i)
    }
    function retain() {
        // Keep instant back navigation, while state outlives older visual trees.
        const recent = [route]
        if (remember) for (const key of recentRoutes) {
            if (key !== route) { recent.push(key); break }
        }
        const next = ({}), states = Object.assign({}, pageStates), artwork = Object.assign({}, artworkStates)
        for (const key of recent) next[key] = true
        for (let i = 0; i < pageLoaders.count; ++i) {
            const loader = pageLoaders.itemAt(i)
            if (loader && isInside(loader.item, transitionSourceItem)) next[loader.routeKey] = true
        }
        // Capture before changing Loader.active: its item is gone afterwards.
        for (let i = 0; i < pageLoaders.count; ++i) {
            const loader = pageLoaders.itemAt(i)
            if (remember && loader && loader.item && !next[loader.routeKey]) {
                if (typeof loader.item.saveNavigationState === "function") states[loader.routeKey] = loader.item.saveNavigationState()
                artwork[loader.routeKey] = artworkFor(loader.item)
            }
        }
        // Acquire Qt's existing pixmaps before unloading their visual owners.
        // These invisible Images have no layers/effects or rendered delegates.
        retainArtwork(artwork)
        artworkStates = artwork
        pageStates = states
        recentRoutes = recent
        visited = next
    }
    onRouteChanged: retain()
    Component.onCompleted: retain()
    ListModel { id: retainedArtwork }
    Item {
        visible: false
        Repeater {
            model: retainedArtwork
            delegate: Image {
                required property string artworkSource
                required property int pixelWidth
                required property int pixelHeight
                objectName: "navigationRetainedArtwork"
                source: artworkSource
                sourceSize: Qt.size(pixelWidth, pixelHeight)
                // Crop mode is part of Qt's pixmap cache key, even when hidden.
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
            }
        }
    }
    Repeater {
        id: pageLoaders
        model: cache.pages
        delegate: Loader {
            required property var modelData
            readonly property string routeKey: modelData.route
            property bool readyForDisplay: false
            anchors.fill: parent
            active: routeKey===cache.route || !!cache.visited[routeKey]
            sourceComponent: modelData.component
            visible: routeKey===cache.route
            opacity: readyForDisplay || cache.pageStates[routeKey] === undefined ? 1 : 0
            enabled: visible && opacity > 0
            onStatusChanged: if (status !== Loader.Ready) readyForDisplay = false
            onLoaded: {
                const state = cache.pageStates[routeKey]
                if (cache.remember && state !== undefined && typeof item.restoreNavigationState === "function")
                    item.restoreNavigationState(state)
                readyForDisplay = true
            }
        }
    }
}
