#include "ui/panorama/panorama_runtime.hpp"

#include "panorama_string_util.hpp"
#include "ui/panorama/panorama_localization.hpp"
#include "ui/panorama/panorama_log.hpp"

#include <quickjs.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace openstrike
{
namespace
{
std::string to_std_string(JSContext* ctx, JSValueConst value)
{
    const char* raw = JS_ToCString(ctx, value);
    std::string result = raw != nullptr ? raw : "";
    if (raw != nullptr)
    {
        JS_FreeCString(ctx, raw);
    }
    return result;
}

// ---- PanoramaNode helpers ----------------------------------------------------

bool node_has_class(const PanoramaNode* node, const std::string& klass)
{
    return node != nullptr && std::find(node->classes.begin(), node->classes.end(), klass) != node->classes.end();
}

bool node_set_class(PanoramaNode* node, const std::string& klass, bool on)
{
    if (node == nullptr || klass.empty())
    {
        return false;
    }
    const auto it = std::find(node->classes.begin(), node->classes.end(), klass);
    if (on && it == node->classes.end())
    {
        node->classes.push_back(klass);
        return true;
    }
    if (!on && it != node->classes.end())
    {
        node->classes.erase(it);
        return true;
    }
    return false;
}

std::string node_attribute(const PanoramaNode* node, const std::string& key, const std::string& fallback)
{
    if (node == nullptr)
    {
        return fallback;
    }
    const auto it = node->attributes.find(key);
    return it != node->attributes.end() ? it->second : fallback;
}

using panorama_strings::to_lower;
using panorama_strings::trim;

bool set_node_attribute(PanoramaNode& node, const std::string& key, std::string value)
{
    auto it = node.attributes.find(key);
    if (it != node.attributes.end() && it->second == value)
    {
        return false;
    }
    node.attributes[key] = std::move(value);
    return true;
}

bool erase_node_attribute(PanoramaNode& node, const std::string& key)
{
    return node.attributes.erase(key) > 0;
}

bool clear_group_selection(PanoramaNode& node, const std::string& group, const PanoramaNode* except = nullptr)
{
    bool changed = false;
    const auto it = node.attributes.find("group");
    if (&node != except && it != node.attributes.end() && it->second == group)
    {
        if (node.selected)
        {
            node.selected = false;
            changed = true;
        }
        changed = node_set_class(&node, "checked", false) || changed;
    }
    for (const auto& child : node.children)
    {
        changed = clear_group_selection(*child, group, except) || changed;
    }
    return changed;
}

bool set_inline_style_property(PanoramaNode& node, const std::string& property, const std::string& value)
{
    const std::string wanted = to_lower(property);
    std::string rebuilt;
    std::size_t cursor = 0;
    while (cursor < node.inline_style.size())
    {
        const std::size_t semi = node.inline_style.find(';', cursor);
        const std::size_t end = semi == std::string::npos ? node.inline_style.size() : semi;
        const std::string declaration = trim(std::string_view(node.inline_style).substr(cursor, end - cursor));
        cursor = semi == std::string::npos ? node.inline_style.size() : semi + 1U;
        const std::size_t colon = declaration.find(':');
        if (colon == std::string::npos || to_lower(trim(std::string_view(declaration).substr(0, colon))) == wanted)
        {
            continue;
        }
        if (!rebuilt.empty())
        {
            rebuilt += ' ';
        }
        rebuilt += declaration;
        rebuilt += ';';
    }
    if (!value.empty())
    {
        if (!rebuilt.empty())
        {
            rebuilt += ' ';
        }
        rebuilt += property;
        rebuilt += ": ";
        rebuilt += value;
        rebuilt += ';';
    }
    if (rebuilt == node.inline_style)
    {
        return false;
    }
    node.inline_style = std::move(rebuilt);
    return true;
}

bool detach_child(PanoramaNode& parent, PanoramaNode& child, std::unique_ptr<PanoramaNode>* out)
{
    for (auto it = parent.children.begin(); it != parent.children.end(); ++it)
    {
        if (it->get() == &child)
        {
            if (out != nullptr)
            {
                *out = std::move(*it);
            }
            parent.children.erase(it);
            return true;
        }
    }
    for (const auto& next : parent.children)
    {
        if (detach_child(*next, child, out))
        {
            return true;
        }
    }
    return false;
}

constexpr const char* kPanelEventMarker = "__panorama_panel_event__";

// The core prelude wires up DOM-agnostic browser conveniences. Game-specific
// native API shims are host-provided bootstrap scripts, matching CEF's model of a
// reusable browser core with embedding-provided handlers.
constexpr const char* kPanoramaCorePrelude = R"JS(
(function () {
    var g = globalThis;

    if (!g.console) {
        g.console = {
            log: function () { $.Msg(Array.prototype.join.call(arguments, ' ')); },
            info: function () { $.Msg(Array.prototype.join.call(arguments, ' ')); },
            warn: function () { $.Warning(Array.prototype.join.call(arguments, ' ')); },
            error: function () { $.Warning(Array.prototype.join.call(arguments, ' ')); },
            assert: function () {}
        };
    }

    var store = {};
    $.persistentStorage = {
        getItem: function (k) { return (k in store) ? store[k] : null; },
        setItem: function (k, v) { store[k] = String(v); },
        removeItem: function (k) { delete store[k]; },
        clear: function () { store = {}; },
        key: function (i) { return Object.keys(store)[i]; },
        get length() { return Object.keys(store).length; }
    };

    $.Each = function (list, fn) {
        if (!list) return;
        if (typeof list.length === 'number') {
            for (var i = 0; i < list.length; i++) fn(list[i], i);
        } else {
            var keys = Object.keys(list);
            for (var j = 0; j < keys.length; j++) fn(list[keys[j]], keys[j]);
        }
    };

    if (!$.LocalizePlural) $.LocalizePlural = function (t) { return $.Localize(t); };
    if (!$.LocalizeSafe) $.LocalizeSafe = function (t, p) { return $.Localize(t, p); };
    if (!$.HTMLEscape) $.HTMLEscape = function (s) { return String(s); };
    if (!$.UrlEncode) $.UrlEncode = function (s) { return encodeURIComponent(String(s)); };
    if (!$.AsyncWebRequest) $.AsyncWebRequest = function () {};
    if (!$.UnregisterForUnhandledEvent) $.UnregisterForUnhandledEvent = function () {};
    if (!$.UnregisterEventHandler) $.UnregisterEventHandler = function () {};
    // Real Panorama: true while a script file is being hot-reloaded in dev mode. Scripts call this
    // at bootstrap (e.g. playercard.js) — leaving it undefined kills their whole init IIFE.
    if (!$.DbgIsReloadingScript) $.DbgIsReloadingScript = function () { return false; };

    $.DispatchEventAsync = function () {
        var args = Array.prototype.slice.call(arguments);
        var delay = args.shift();
        $.Schedule(delay, function () { $.DispatchEvent.apply($, args); });
    };
    $.FindChildInContext = function (id) {
        var ctx = $.GetContextPanel();
        return ctx ? ctx.FindChildTraverse(id) : null;
    };

    function makeStub(name) {
        var target = function () {};
        return new Proxy(target, {
            get: function (t, prop) {
                if (prop === Symbol.toPrimitive)
                    return function (hint) { return hint === 'number' ? 0 : ''; };
                if (prop === 'toString' || prop === 'valueOf')
                    return function () { return ''; };
                if (prop === Symbol.iterator)
                    return function () { return [][Symbol.iterator](); };
                if (prop === 'length') return 0;
                if (prop === 'then') return undefined;
                return makeStub(name + '.' + String(prop));
            },
            apply: function () { return makeStub(name + '()'); },
            construct: function () { return makeStub('new ' + name); }
        });
    }
    g.__PanoramaStub = makeStub;
})();
)JS";

constexpr const char* kPanoramaCsgoHostPreludePart1 = R"JS(
(function () {
    var g = globalThis;
    var makeStub = g.__PanoramaStub;
    if (!makeStub) {
        makeStub = function (name) {
            var target = function () {};
            return new Proxy(target, {
                get: function (t, prop) {
                    if (prop === Symbol.toPrimitive)
                        return function (hint) { return hint === 'number' ? 0 : ''; };
                    if (prop === 'toString' || prop === 'valueOf')
                        return function () { return ''; };
                    if (prop === Symbol.iterator)
                        return function () { return [][Symbol.iterator](); };
                    if (prop === 'length') return 0;
                    if (prop === 'then') return undefined;
                    return makeStub(name + '.' + String(prop));
                },
                apply: function () { return makeStub(name + '()'); },
                construct: function () { return makeStub('new ' + name); }
            });
        };
        g.__PanoramaStub = makeStub;
    }

    var apis = ['UiToolkitAPI', 'GameInterfaceAPI', 'RInputAPI', 'SteamOverlayAPI',
        'MyPersonaAPI', 'FriendsListAPI', 'PartyListAPI', 'LobbyAPI', 'InventoryAPI',
        'GameStateAPI', 'GameTypesAPI', 'OptionsMenuAPI', 'EmoticonServices',
        'CompetitiveMatchAPI', 'FantasyAPI', 'WebAPI', 'TournamentAPI', 'GameEventsAPI',
        'ChatAPI', 'NewsAPI', 'StoreAPI', 'ItemSetCharacterAPI', 'VideoAPI',
        'GameInstructorAPI', 'RankAndMedalAPI', 'LocalInventoryAPI', 'MatchListAPI',
        'CSGOOperationsAPI', 'CSGOPlayerListAPI', 'TournamentsAPI', 'MatchStatsAPI',
        'PromotedSettingsUtil', 'WorkshopAPI', 'MissionsAPI', 'Scheduler', 'CharacterAnims'];
    for (var a = 0; a < apis.length; a++) {
        if (typeof g[apis[a]] === 'undefined') g[apis[a]] = makeStub(apis[a]);
    }

    // Host-backed APIs: real methods that reach the engine via $.__host, with a
    // stub fallback for everything else. Lets the menu run console commands and
    // start a match even though the rest of matchmaking is stubbed.
    function hostBacked(name, methods) {
        return new Proxy(methods, {
            get: function (t, p) { return (p in t) ? t[p] : makeStub(name + '.' + String(p)); }
        });
    }
    var settingsStore = {
        ui_mainmenu_bkgnd_movie: 'nuke720p',
        ui_playsettings_prime: '0',
        ui_playsettings_mode_official: 'competitive',
        ui_playsettings_mode_listen: 'competitive',
        ui_playsettings_maps_official_competitive: 'mg_lobby_mapveto,mg_de_ancient,mg_de_anubis,mg_de_inferno,mg_de_mirage,mg_de_nuke,mg_de_overpass,mg_de_vertigo',
        ui_playsettings_maps_official_casual: 'mg_casualsigma,mg_casualdelta',
        ui_playsettings_maps_official_deathmatch: 'mg_deathmatch',
        ui_playsettings_maps_official_scrimcomp2v2: 'mg_de_boyard,mg_de_chalice,mg_de_vertigo,mg_de_inferno,mg_de_overpass',
        ui_playsettings_maps_official_skirmish: 'mg_skirmish_armsrace',
        ui_playsettings_maps_official_survival: 'mg_dz_blacksite',
        ui_playsettings_maps_official_cooperative: 'mg_coop_kasbah',
        ui_playsettings_maps_official_coopmission: 'mg_coop_cementplant',
        ui_playsettings_maps_listen_competitive: 'mg_de_ancient',
        ui_playsettings_maps_listen_casual: 'mg_de_dust2',
        ui_playsettings_maps_listen_deathmatch: 'mg_de_dust2',
        ui_playsettings_maps_listen_scrimcomp2v2: 'mg_de_boyard',
        ui_playsettings_maps_listen_skirmish: 'mg_skirmish_armsrace',
        ui_playsettings_maps_listen_survival: 'mg_dz_blacksite',
        ui_playsettings_maps_listen_cooperative: 'mg_coop_kasbah',
        ui_playsettings_maps_listen_coopmission: 'mg_coop_cementplant',
        ui_playsettings_flags_official_competitive: '0',
        ui_playsettings_flags_listen_competitive: '0',
        ui_show_unlock_competitive_alert: '1',
        lobby_default_privacy_bits: '0',
        player_botdifflast_s: '1'
    };
    var callbackStore = {};
    var nextCallbackId = 1;
    var sessionActive = false;
    var sessionSettings = {
        system: { access: 'private' },
        options: {
            action: 'custommatch',
            server: 'official',
            challengekey: ''
        },
        game: {
            mode: 'competitive',
            type: 'classic',
            mapgroupname: 'mg_lobby_mapveto',
            gamemodeflags: 0,
            prime: 0
        }
    };
    function shallowMerge(target, source) {
        if (!source) return target;
        Object.keys(source).forEach(function (k) {
            if (source[k] && typeof source[k] === 'object' && !Array.isArray(source[k])) {
                target[k] = shallowMerge(target[k] || {}, source[k]);
            } else {
                target[k] = source[k];
            }
        });
        return target;
    }

    // Resolves a `mapgroupname` (a single group like 'mg_de_mirage', a comma list of
    // groups, or a preset name) to a concrete bsp map name (e.g. 'de_mirage') the
    // engine can load. The play menu stores the selection as mapgroupname(s); the host
    // `play` action needs an actual map. Picks the first map of the first valid group.
    function resolveMapFromGroupName(mapgroupname) {
        if (!mapgroupname) return '';
        var groups;
        try { groups = g.GameTypesAPI.GetConfig().mapgroups; } catch (e) { groups = null; }
        var names = String(mapgroupname).split(',');
        for (var i = 0; i < names.length; ++i) {
            var name = names[i].trim();
            if (!name || name.indexOf('random_') === 0) continue;
            var group = groups ? groups[name] : null;
            if (group && group.maps) {
                var keys = Object.keys(group.maps);
                if (keys.length > 0) return keys[0];
            }
            // No config entry: fall back to stripping a leading 'mg_' (single-map group).
            if (name.indexOf('mg_') === 0) return name.substring(3);
        }
        return '';
    }

    g.GameInterfaceAPI = hostBacked('GameInterfaceAPI', {
        ConsoleCommand: function (c) { $.__host('cmd', String(c)); },
        RunConVarCommand: function (c) { $.__host('cmd', String(c)); },
        GetSettingString: function (k, fallback) {
            return Object.prototype.hasOwnProperty.call(settingsStore, k) ? settingsStore[k] : (fallback !== undefined ? String(fallback) : '');
        },
        SetSettingString: function (k, v) { settingsStore[k] = String(v); },
        GetEngineSoundSystemsRunning: function () { return true; }
    });
    // Launch the session's selected map AND game mode. The play menu keeps the
    // selection in the lobby session settings (mainmenu_play.js _ApplySessionSettings
    // sends Game.mode/Game.type via UpdateSessionSettings); the host parses
    // "<map> <mode>" and maps the mode name onto the game_type/game_mode convars.
    function hostPlay() {
        var map = resolveMapFromGroupName(sessionSettings.game.mapgroupname);
        var mode = String(sessionSettings.game.mode || '');
        $.__host('play', mode ? map + ' ' + mode : map);
    }

    g.LobbyAPI = hostBacked('LobbyAPI', {
        IsSessionActive: function () { return sessionActive; },
        CreateSession: function () { sessionActive = true; return true; },
        GetSessionSettings: function () { return sessionSettings; },
        UpdateSessionSettings: function (settings) {
            if (settings && settings.update) {
                if (settings.update.System) shallowMerge(sessionSettings.system, settings.update.System);
                if (settings.update.Options) shallowMerge(sessionSettings.options, settings.update.Options);
                if (settings.update.Game) shallowMerge(sessionSettings.game, settings.update.Game);
            }
            if (settings && settings.delete && settings.delete.Options) {
                Object.keys(settings.delete.Options).forEach(function (key) { delete sessionSettings.options[key]; });
            }
            sessionActive = true;
            $.DispatchEvent('PanoramaComponent_Lobby_MatchmakingSessionUpdate', 'updated');
            return true;
        },
        BIsHost: function () { return true; },
        StopMatchmaking: function () {},
        GetMatchmakingStatusString: function () { return ''; },
        GetMapWaitTimeInSeconds: function () { return 0; },
        LaunchTrainingMap: function () { $.__host('play', 'training1 training'); },
        StartMatchmaking: function () { hostPlay(); return true; },
        StartCustomMatch: function () { hostPlay(); return true; }
    });
)JS";

constexpr const char* kPanoramaCsgoHostPreludePart2 = R"JS(    g.GameTypesAPI = hostBacked('GameTypesAPI', {
        GetConfig: function () {
            function mapGroup(keys) {
                var result = {};
                keys.forEach(function (key, index) { result[key] = String(index); });
                return result;
            }
            function singleMapGroup(map, extra) {
                var result = shallowMerge({
                    imagename: 'map-' + map.replace(/^(de|cs|dz|ar)_/, '') + '-overall',
                    nameID: '#SFUI_Map_' + map,
                    name: 'mg_' + map,
                    icon_image_path: 'map_icons/map_icon_' + map,
                    maps: {}
                }, extra || {});
                result.maps[map] = {};
                return result;
            }
            var activeDuty = ['mg_de_ancient', 'mg_de_anubis', 'mg_de_inferno', 'mg_de_mirage', 'mg_de_nuke', 'mg_de_overpass', 'mg_de_vertigo'];
            var reserves = ['mg_de_tuscan', 'mg_de_dust2', 'mg_de_train', 'mg_de_cache'];
            var casualGroups = ['mg_casualsigma', 'mg_casualdelta'];
            var classicMaps = ['mg_de_dust2', 'mg_de_mirage', 'mg_de_inferno', 'mg_de_vertigo', 'mg_de_anubis', 'mg_de_ancient', 'mg_de_train', 'mg_de_overpass', 'mg_de_nuke', 'mg_de_tuscan', 'mg_cs_office', 'mg_cs_italy'];
            var wingmanMaps = ['mg_de_boyard', 'mg_de_chalice', 'mg_de_vertigo', 'mg_de_inferno', 'mg_de_overpass', 'mg_de_train', 'mg_de_shortnuke', 'mg_de_shortdust', 'mg_de_lake'];
            return {
                gameTypes: {
                    classic: {
                        gameModes: {
                            casual: { mapgroupsMP: mapGroup(casualGroups), mapgroupsSP: mapGroup(classicMaps) },
                            competitive: { mapgroupsMP: mapGroup(['mg_lobby_mapveto'].concat(activeDuty, reserves)), mapgroupsSP: mapGroup(activeDuty.concat(reserves)) },
                            scrimcomp2v2: { mapgroupsMP: mapGroup(wingmanMaps), mapgroupsSP: mapGroup(wingmanMaps) },
                            deathmatch: { mapgroupsMP: mapGroup(casualGroups), mapgroupsSP: mapGroup(classicMaps) }
                        }
                    },
                    skirmish: {
                        gameModes: {
                            skirmish: { mapgroupsMP: mapGroup(['mg_skirmish_armsrace', 'mg_skirmish_demolition', 'mg_skirmish_flyingscoutsman', 'mg_skirmish_retakes']), mapgroupsSP: mapGroup(['mg_skirmish_armsrace', 'mg_skirmish_demolition', 'mg_skirmish_flyingscoutsman', 'mg_skirmish_retakes']) }
                        }
                    },
                    freeforall: {
                        gameModes: {
                            survival: { mapgroupsMP: mapGroup(['mg_dz_blacksite', 'mg_dz_sirocco', 'mg_dz_county', 'mg_dz_ember']), mapgroupsSP: mapGroup(['mg_dz_blacksite', 'mg_dz_sirocco', 'mg_dz_county', 'mg_dz_ember']) }
                        }
                    },
                    cooperative: {
                        gameModes: {
                            cooperative: { mapgroupsMP: mapGroup(['mg_coop_kasbah', 'mg_coop_cementplant']), mapgroupsSP: mapGroup(['mg_coop_kasbah', 'mg_coop_cementplant']) },
                            coopmission: { mapgroupsMP: mapGroup(['mg_coop_cementplant']), mapgroupsSP: mapGroup(['mg_coop_cementplant']) }
                        }
                    },
                    training: {
                        gameModes: {
                            training: { mapgroupsMP: mapGroup(['mg_training1']), mapgroupsSP: mapGroup(['mg_training1']) }
                        }
                    }
                },
                mapgroups: {
                    mg_lobby_mapveto: {
                        imagename: 'map-lobby-mapveto-overall',
                        nameID: '#SFUI_Map_lobby_mapveto',
                        name: 'mg_lobby_mapveto',
                        icon_image_path: 'map_icons/map_icon_lobby_mapveto',
                        maps: { de_ancient: {}, de_anubis: {}, de_inferno: {}, de_mirage: {}, de_nuke: {}, de_overpass: {}, de_vertigo: {} },
                        grouptype: 'active'
                    },
                    mg_casualsigma: {
                        imagename: 'mapgroup-casualsigma',
                        nameID: '#SFUI_Mapgroup_casualsigma',
                        name: 'mg_casualsigma',
                        icon_image_path: 'map_icons/mapgroup_icon_reserves',
                        maps: { de_cache: {}, de_ancient: {}, de_train: {}, de_overpass: {}, de_nuke: {}, de_tuscan: {} }
                    },
                    mg_casualdelta: {
                        imagename: 'mapgroup-casualdelta',
                        nameID: '#SFUI_Mapgroup_casualdelta',
                        name: 'mg_casualdelta',
                        icon_image_path: 'map_icons/mapgroup_icon_reserves',
                        maps: { de_mirage: {}, de_inferno: {}, de_vertigo: {}, de_cbble: {}, de_anubis: {} }
                    },
                    mg_deathmatch: {
                        imagename: 'mapgroup-bomb',
                        nameID: '#SFUI_Mapgroup_allclassic',
                        name: 'mg_deathmatch',
                        icon_image_path: 'map_icons/mapgroup_icon_deathmatch',
                        maps: { de_dust2: {}, de_mirage: {}, de_inferno: {}, de_vertigo: {}, de_ancient: {}, de_cache: {}, de_anubis: {}, de_train: {}, de_overpass: {}, de_nuke: {} }
                    },
                    mg_de_ancient: singleMapGroup('de_ancient', { grouptype: 'active' }),
                    mg_de_anubis: singleMapGroup('de_anubis', { grouptype: 'active' }),
                    mg_de_inferno: singleMapGroup('de_inferno', { grouptype: 'active' }),
                    mg_de_mirage: singleMapGroup('de_mirage', { grouptype: 'active' }),
                    mg_de_nuke: singleMapGroup('de_nuke', { grouptype: 'active' }),
                    mg_de_overpass: singleMapGroup('de_overpass', { grouptype: 'active' }),
                    mg_de_vertigo: singleMapGroup('de_vertigo', { grouptype: 'active' }),
                    mg_de_tuscan: singleMapGroup('de_tuscan'),
                    mg_de_dust2: singleMapGroup('de_dust2'),
                    mg_de_train: singleMapGroup('de_train'),
                    mg_de_cache: singleMapGroup('de_cache'),
                    mg_de_boyard: singleMapGroup('de_boyard'),
                    mg_de_chalice: singleMapGroup('de_chalice'),
                    mg_de_shortnuke: singleMapGroup('de_shortnuke', { icon_image_path: 'map_icons/map_icon_de_nuke', maps: { de_shortnuke: {} } }),
                    mg_de_shortdust: singleMapGroup('de_shortdust'),
                    mg_de_lake: singleMapGroup('de_lake'),
                    mg_cs_office: singleMapGroup('cs_office'),
                    mg_cs_italy: singleMapGroup('cs_italy'),
                    mg_skirmish_armsrace: {
                        imagename: 'mapgroup-armsrace',
                        nameID: '#Skirmish_AR_name',
                        name: 'mg_skirmish_armsrace',
                        icon_image_path: 'map_icons/mapgroup_icon_skirmish',
                        maps: { ar_baggage: {}, ar_shoots: {}, ar_lunacy: {} }
                    },
                    mg_skirmish_demolition: {
                        imagename: 'mapgroup-demolition',
                        nameID: '#Skirmish_DEM_name',
                        name: 'mg_skirmish_demolition',
                        icon_image_path: 'map_icons/mapgroup_icon_skirmish',
                        maps: { de_lake: {}, de_bank: {}, de_safehouse: {}, de_stmarc: {} }
                    },
                    mg_skirmish_flyingscoutsman: {
                        imagename: 'mapgroup-flyingscoutsman',
                        nameID: '#Skirmish_CC_FS_name',
                        name: 'mg_skirmish_flyingscoutsman',
                        icon_image_path: 'map_icons/mapgroup_icon_skirmish',
                        maps: { de_dust2: {}, de_train: {}, de_lake: {} }
                    },
                    mg_skirmish_retakes: {
                        imagename: 'mapgroup-retakes',
                        nameID: '#Skirmish_CC_RT_name',
                        name: 'mg_skirmish_retakes',
                        icon_image_path: 'map_icons/mapgroup_icon_skirmish',
                        maps: { de_dust2: {}, de_mirage: {}, de_inferno: {}, de_nuke: {} }
                    },
                    mg_dz_blacksite: singleMapGroup('dz_blacksite'),
                    mg_dz_sirocco: singleMapGroup('dz_sirocco'),
                    mg_dz_county: singleMapGroup('dz_county'),
                    mg_dz_ember: singleMapGroup('dz_ember'),
                    mg_coop_kasbah: singleMapGroup('coop_kasbah', { icon_image_path: 'map_icons/map_icon_dz_sirocco' }),
                    mg_coop_cementplant: singleMapGroup('coop_cementplant', { icon_image_path: 'map_icons/map_icon_coop_cementplant' }),
                    mg_training1: {
                        nameID: '#SFUI_Map_training1',
                        name: 'mg_training1',
                        icon_image_path: 'map_icons/map_icon_training1',
                        maps: { training1: {} }
                    }
                }
            };
        },
        GetMapGroupAttribute: function (name, attr) {
            var group = this.GetConfig().mapgroups[name];
            return group && group[attr] !== undefined ? group[attr] : '';
        },
        SetCustomBotDifficulty: function () {}
    });
)JS";

constexpr const char* kPanoramaCsgoHostPreludePart3 = R"JS(    g.MyPersonaAPI = hostBacked('MyPersonaAPI', {
        GetLauncherType: function () { return ''; },
        IsInventoryValid: function () { return true; },
        IsConnectedToGC: function () { return true; },
        HasPrestige: function () { return true; },
        GetCurrentLevel: function () { return 40; },
        GetXuid: function () { return '1'; },
        GetLicenseType: function () { return 'purchased'; },
        GetMyClanCount: function () { return 0; },
        GetMyClanNameById: function () { return ''; },
        GetMyOfficialTournamentName: function () { return ''; },
        GetMyOfficialTeamName: function () { return ''; },
        GetClientLogonFatalError: function () { return ''; },
        GetTimePlayedTrackingState: function () { return 0; },
        HintLoadPipRanks: function () {},
        ActionBuyLicense: function () {}
    });
    g.GameStateAPI = hostBacked('GameStateAPI', {
        IsQueuedMatchmaking: function () { return false; },
        IsTraining: function () { return false; },
        IsGotvSpectating: function () { return false; },
        GetActiveQuestID: function () { return 0; },
        GetTimeDataJSO: function () { return {}; }
    });
    // Generic confirmation/notice popups. CS:GO routes these to a native popup
    // layer the host doesn't have, so we build a real modal overlay out of plain
    // panels (the same approach as the host's playtest notice) and wire the option
    // buttons to the caller's callbacks. Without this the main-menu Quit button —
    // whose whole action is ShowGenericPopupTwoOptionsBgStyle — does nothing.
    function _popupLoc(s) {
        s = String(s == null ? '' : s);
        return (s.charAt(0) === '#') ? $.Localize(s) : s;
    }
    function _showGenericPopup(title, msg, buttons) {
        var anchor = $.GetContextPanel();
        if (!anchor) { return makeStub('Popup'); }
        var root = anchor;
        while (root.GetParent && root.GetParent()) { root = root.GetParent(); }
        var overlay = $.CreatePanel('Panel', root, '', {
            style: 'width: 100%; height: 100%; background-color: rgba(0, 0, 0, 0.65); z-index: 10000;'
        });
        var box = $.CreatePanel('Panel', overlay, '', {
            style: 'width: 540px; flow-children: down; align: center center; padding: 30px 38px; ' +
                'background-color: rgba(22, 26, 31, 0.98); border: 1px solid rgba(255, 255, 255, 0.16); ' +
                'border-radius: 4px; box-shadow: 0px 0px 28px 6px rgba(0, 0, 0, 0.85);'
        });
        $.CreatePanel('Label', box, '', {
            text: _popupLoc(title),
            style: 'horizontal-align: center; color: rgb(234, 234, 234); font-size: 26px; ' +
                'font-weight: bold; letter-spacing: 1px; margin-bottom: 14px;'
        });
        $.CreatePanel('Label', box, '', {
            text: _popupLoc(msg),
            style: 'horizontal-align: center; text-align: center; color: rgb(189, 195, 199); ' +
                'font-size: 18px; margin-bottom: 26px; width: 100%;'
        });
        var row = $.CreatePanel('Panel', box, '', { style: 'flow-children: right; horizontal-align: center;' });
        function close() { if (overlay && overlay.DeleteAsync) { overlay.DeleteAsync(0.0); } }
        for (var i = 0; i < buttons.length; i++) {
            (function (b, idx) {
                var btn = $.CreatePanel('Button', row, '', {
                    style: 'margin: 0px 8px; padding: 9px 32px; border-radius: 2px; ' +
                        'border: 1px solid rgba(255, 255, 255, 0.25); ' +
                        'background-color: rgba(255, 255, 255, ' + (idx === 0 ? '0.18' : '0.10') + ');'
                });
                $.CreatePanel('Label', btn, '', {
                    text: _popupLoc(b.text),
                    style: 'color: rgb(234, 234, 234); font-size: 16px; letter-spacing: 1px;'
                });
                btn.SetPanelEvent('onactivate', function () {
                    close();
                    if (typeof b.cb === 'function') { try { b.cb(); } catch (e) { $.Warning('popup callback: ' + e); } }
                });
            })(buttons[i], i);
        }
        return makeStub('Popup');
    }
    g.UiToolkitAPI = hostBacked('UiToolkitAPI', {
        IsPanoramaInECOMode: function () { return false; },
        RegisterJSCallback: function (fn) {
            var id = String(nextCallbackId++);
            callbackStore[id] = fn;
            return id;
        },
        UnregisterJSCallback: function (id) { delete callbackStore[String(id)]; },
        InvokeJSCallback: function (id) {
            var fn = callbackStore[String(id)];
            if (typeof fn === 'function') {
                return fn.apply(null, Array.prototype.slice.call(arguments, 1));
            }
            return undefined;
        },
        HideTextTooltip: function () {},
        ShowTextTooltip: function () {},
        HideCustomLayoutTooltip: function () {},
        CloseAllVisiblePopups: function () {},
        ShowCustomLayoutPopup: function () { return makeStub('Popup'); },
        ShowCustomLayoutPopupParameters: function () { return makeStub('Popup'); },
        ShowCustomLayoutParametersTooltip: function () { return makeStub('Tooltip'); },
        ShowCustomLayoutContextMenuParametersDismissEvent: function () { return makeStub('ContextMenu'); },
        ShowGenericPopup: function (title, msg) { return _showGenericPopup(title, msg, [{ text: 'OK' }]); },
        ShowGenericPopupOk: function (title, msg, style, okCb) {
            return _showGenericPopup(title, msg, [{ text: 'OK', cb: okCb }]);
        },
        ShowGenericPopupOkCancel: function (title, msg, style, okCb, cancelCb) {
            return _showGenericPopup(title, msg, [{ text: 'OK', cb: okCb }, { text: 'Cancel', cb: cancelCb }]);
        },
        ShowGenericPopupYesNo: function (title, msg, style, yesCb, noCb) {
            return _showGenericPopup(title, msg, [{ text: 'Yes', cb: yesCb }, { text: 'No', cb: noCb }]);
        },
        ShowGenericPopupOneOption: function (title, msg, style, t1, c1) {
            return _showGenericPopup(title, msg, [{ text: t1, cb: c1 }]);
        },
        ShowGenericPopupOneOptionBgStyle: function (title, msg, style, t1, c1) {
            return _showGenericPopup(title, msg, [{ text: t1, cb: c1 }]);
        },
        ShowGenericPopupTwoOptions: function (title, msg, style, t1, c1, t2, c2) {
            return _showGenericPopup(title, msg, [{ text: t1, cb: c1 }, { text: t2, cb: c2 }]);
        },
        ShowGenericPopupTwoOptionsBgStyle: function (title, msg, style, t1, c1, t2, c2) {
            return _showGenericPopup(title, msg, [{ text: t1, cb: c1 }, { text: t2, cb: c2 }]);
        },
        ShowGenericPopupThreeOptionsBgStyle: function (title, msg, style, t1, c1, t2, c2, t3, c3) {
            return _showGenericPopup(title, msg, [{ text: t1, cb: c1 }, { text: t2, cb: c2 }, { text: t3, cb: c3 }]);
        }
    });
    g.PromotedSettingsUtil = hostBacked('PromotedSettingsUtil', {
        GetUnacknowledgedPromotedSettings: function () { return []; }
    });
    g.PartyListAPI = hostBacked('PartyListAPI', {
        GetCount: function () { return 1; },
        GetXuidByIndex: function () { return '1'; },
        GetFriendPrimeEligible: function () { return true; },
        GetPartySessionUiThreshold: function () { return 2; },
        GetPartySystemSetting: function (key) { return key === 'xuidHost' ? '1' : ''; }
    });
    g.FriendsListAPI = hostBacked('FriendsListAPI', {
        GetFriendName: function (xuid) { return String(xuid || '1') === '1' ? 'Player' : 'Friend'; }
    });
    g.CompetitiveMatchAPI = hostBacked('CompetitiveMatchAPI', {
        GetDirectChallengeCode: function () { return ''; },
        GenerateDirectChallengeCode: function () { return ''; },
        ValidateDirectChallengeCode: function () { return ''; },
        GetTournamentTeamCount: function () { return 0; },
        GetTournamentTeamNameByIndex: function () { return ''; },
        GetTournamentStageCount: function () { return 0; },
        GetTournamentStageNameByIndex: function () { return ''; },
        GetRotatingOfficialMapGroupCurrentState: function () { return ''; }
    });
    g.PartyBrowserAPI = hostBacked('PartyBrowserAPI', {
        GetPartyMembersCount: function () { return 0; },
        GetPartyMemberXuid: function () { return ''; },
        GetPrivateQueuesCount: function () { return 0; },
        GetPrivateQueuesPlayerCount: function () { return 0; },
        GetPrivateQueuesMoreParties: function () { return 0; },
        GetPrivateQueuePartyXuidByIndex: function () { return ''; }
    });
    g.WorkshopAPI = hostBacked('WorkshopAPI', {
        GetNumSubscribedMaps: function () { return 0; },
        GetSubscribedMapID: function () { return ''; },
        GetWorkshopMapInfo: function () { return {}; }
    });
    g.SteamOverlayAPI = hostBacked('SteamOverlayAPI', {
        IsEnabled: function () { return false; },
        OpenURL: function () {},
        OpenUrlInOverlayOrExternalBrowser: function () {},
        GetSteamCommunityURL: function () { return 'https://steamcommunity.com'; },
        CopyTextToClipboard: function () {}
    });
    g.StoreAPI = hostBacked('StoreAPI', {
        GetStoreItemSalePrice: function () { return ''; }
    });
    g.InventoryAPI = hostBacked('InventoryAPI', {
        GetFauxItemIDFromDefAndPaintIndex: function () { return '0'; },
        GetCacheTypeElementFieldByIndex: function () { return 0; },
        SetInventorySortAndFilters: function () {}
    });
    g.MissionsAPI = hostBacked('MissionsAPI', {
        GetQuestDefinitionField: function () { return ''; },
        ApplyQuestDialogVarsToPanelJS: function () {}
    });
    g.LeaderboardsAPI = hostBacked('LeaderboardsAPI', {
        Refresh: function () {}
    });
    // Gating APIs must return falsy/0 for the normal path; the default stub returns
    // a truthy object, which sends `if(API.X())` checks down the wrong branch (e.g.
    // _BCheckTabCanBeOpenedRightNow aborts tab navigation). Return real values.
    g.MatchStatsAPI = hostBacked('MatchStatsAPI', {
        GetUiExperienceType: function () { return 0; }
    });

    try {
        var globalFallback = new Proxy(Object.prototype, {
            has: function () { return true; },
            get: function (t, prop) {
                if (prop in Object.prototype) return Object.prototype[prop];
                if (typeof prop === 'symbol') return undefined;
                return makeStub(String(prop));
            }
        });
        Object.setPrototypeOf(g, globalFallback);
    } catch (e) { $.Warning('global fallback unavailable: ' + e); }

    var sample = $.GetContextPanel();
    if (sample) {
        var panelProto = Object.getPrototypeOf(sample);
        var fallback = new Proxy(Object.prototype, {
            get: function (t, prop) {
                if (typeof prop === 'symbol' || prop in Object.prototype) return Object.prototype[prop];
                return function () { return makeStub('Panel.' + String(prop)); };
            }
        });
        Object.setPrototypeOf(panelProto, fallback);
    }
})();
)JS";

std::string make_panorama_csgo_host_prelude_source()
{
    std::string source;
    source.reserve(
        std::strlen(kPanoramaCsgoHostPreludePart1) +
        std::strlen(kPanoramaCsgoHostPreludePart2) +
        std::strlen(kPanoramaCsgoHostPreludePart3));
    source += kPanoramaCsgoHostPreludePart1;
    source += kPanoramaCsgoHostPreludePart2;
    source += kPanoramaCsgoHostPreludePart3;
    return source;
}
}

//=============================================================================
// PanoramaRuntime::Impl
//=============================================================================

struct PanoramaRuntime::Impl final : PanoramaNodeLifetimeObserver
{
    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr;
    JSClassID panel_class_id = 0;
    JSClassID style_class_id = 0;

    PanoramaNode* document = nullptr;
    bool dirty = false;

    std::function<void(PanoramaNode&, const std::string&)> load_layout_file;
    std::function<void(PanoramaNode&, const std::string&)> load_layout_snippet;
    std::function<bool(const std::string&)> has_layout_snippet;
    std::function<void(const std::string&, const std::string&)> host_action;
    PanoramaRuntimeClient* client = nullptr;
    PanoramaLocalization localization;
    std::vector<PanoramaRuntimeScript> bootstrap_scripts;

    // Script-context stack. Every script execution (layout include, event
    // handler, scheduled callback) pushes the context panel of the script that
    // registered it, so $.GetContextPanel() resolves to that script's layout
    // root (real Panorama semantics), not the document. A stack — not a
    // save/restore local — so node destruction can null dead entries in place
    // (a restored local would re-arm a dangling pointer).
    std::vector<PanoramaNode*> context_stack;

    // Innermost live context, no fallback: what a registration should capture.
    [[nodiscard]] PanoramaNode* innermost_context() const
    {
        for (auto it = context_stack.rbegin(); it != context_stack.rend(); ++it)
        {
            if (*it != nullptr)
            {
                return *it;
            }
        }
        return nullptr;
    }

    // Innermost live context with the document fallback: what $.GetContextPanel
    // and context-relative lookups should resolve against.
    [[nodiscard]] PanoramaNode* current_context() const
    {
        PanoramaNode* context = innermost_context();
        return context != nullptr ? context : document;
    }

    struct EventHandler
    {
        JSValue fn = JS_UNDEFINED;
        PanoramaNode* context = nullptr;
        // Panel the handler was scoped to: RegisterEventHandler(event, panel, fn)
        // in real Panorama only fires for events ON that panel (teamselectmenu.js
        // relies on this — its PropertyTransitionEnd handler doesn't compare
        // panelName). Null = unscoped (RegisterForUnhandledEvent / 2-arg form).
        PanoramaNode* target = nullptr;
    };
    std::unordered_map<std::string, std::vector<EventHandler>> event_handlers;

    // One stable JS wrapper per node (script-side identity: wrapping the same
    // panel twice yields `===` objects). The map holds its own reference; node
    // destruction nulls the wrapper's opaque (so surviving script references
    // become inert — every panel method treats a null opaque as "panel gone")
    // and releases the reference.
    std::unordered_map<PanoramaNode*, JSValue> panel_wrappers;
    std::unordered_map<PanoramaNode*, JSValue> panel_data;
    std::unordered_map<PanoramaNode*, std::unordered_map<std::string, JSValue>> panel_events;
    std::unordered_map<PanoramaNode*, std::unordered_map<std::string, std::string>> switch_classes;

    struct Scheduled
    {
        int id = 0;
        double remaining = 0.0;
        JSValue fn = JS_UNDEFINED;
        PanoramaNode* context = nullptr; // registering script's context panel
    };
    std::vector<Scheduled> scheduled;
    int next_schedule_id = 1;

    // In-flight dispatch snapshots (fire()'s handler copy, update()'s due list).
    // A handler may delete panels mid-dispatch; node destruction must null dead
    // contexts inside these working copies too, not just the registries above.
    std::vector<std::vector<EventHandler>*> live_handler_batches;
    std::vector<Scheduled>* firing_scheduled = nullptr;

    // SetDialogVariable: capture each node's original text as a template so repeated
    // updates re-expand cleanly into the node's text.
    std::unordered_map<PanoramaNode*, std::string> dialog_templates;
    std::unordered_map<PanoramaNode*, std::unordered_map<std::string, std::string>> dialog_vars;

    JSValue wrap_panel(PanoramaNode* node);
    PanoramaNode* panel_node(JSValueConst value) const;

    // `source` scopes delivery: handlers registered with a panel target only run
    // when that panel is the event's source. A null source (plain DispatchEvent)
    // keeps the historical behavior of invoking every handler.
    void fire(const std::string& name, int argc, JSValueConst* argv, PanoramaNode* source = nullptr);
    void run_code(PanoramaNode* context, const std::string& code, const char* origin);
    void apply_dialog_variables(PanoramaNode* node);
    void dump_pending_error(const char* origin);

    // PanoramaNodeLifetimeObserver: drop every reference to a dying node before
    // it dangles (registered for the lifetime of the JS context).
    void on_panorama_node_destroyed(PanoramaNode& node) override;

    ~Impl() override
    {
        panorama_remove_node_lifetime_observer(*this);
    }

    // Pushes a script context for the duration of a scope (layout script, event
    // handler, scheduled callback).
    struct ContextScope
    {
        Impl& impl;
        ContextScope(Impl& owner, PanoramaNode* context) : impl(owner)
        {
            impl.context_stack.push_back(context);
        }
        ~ContextScope()
        {
            impl.context_stack.pop_back();
        }
        ContextScope(const ContextScope&) = delete;
        ContextScope& operator=(const ContextScope&) = delete;
    };
};

namespace
{
PanoramaRuntime::Impl* impl_of(JSContext* ctx)
{
    return static_cast<PanoramaRuntime::Impl*>(JS_GetContextOpaque(ctx));
}

PanoramaNode* node_arg(JSContext* ctx, JSValueConst value)
{
    return impl_of(ctx)->panel_node(value);
}

// Hidden slot on style objects holding the owning panel wrapper. Styles resolve
// their node through the wrapper at every access instead of caching the raw
// pointer, so a style object held across its panel's deletion goes inert (the
// wrapper's opaque is nulled on destruction) instead of dangling.
constexpr const char* kStylePanelSlot = "__panel";

PanoramaNode* style_node_arg(JSContext* ctx, JSValueConst value)
{
    JSValue panel = JS_GetPropertyStr(ctx, value, kStylePanelSlot);
    PanoramaNode* node = impl_of(ctx)->panel_node(panel);
    JS_FreeValue(ctx, panel);
    return node;
}

JSValue wrap(JSContext* ctx, PanoramaNode* node)
{
    return impl_of(ctx)->wrap_panel(node);
}

void mark_dirty(JSContext* ctx)
{
    impl_of(ctx)->dirty = true;
}

void mark_dirty_if(JSContext* ctx, bool changed)
{
    if (changed)
    {
        mark_dirty(ctx);
    }
}

void set_selected_state(JSContext* ctx, PanoramaNode* node, bool selected)
{
    if (node == nullptr)
    {
        return;
    }
    bool changed = false;
    if (selected)
    {
        const auto group = node->attributes.find("group");
        if (group != node->attributes.end() && impl_of(ctx)->document != nullptr)
        {
            changed = clear_group_selection(*impl_of(ctx)->document, group->second, node) || changed;
        }
    }
    if (node->selected != selected)
    {
        node->selected = selected;
        changed = true;
    }
    changed = node_set_class(node, "checked", selected) || changed;
    mark_dirty_if(ctx, changed);
}

//--- Panel methods -----------------------------------------------------------

JSValue panel_add_class(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    if (node != nullptr && argc > 0)
    {
        mark_dirty_if(ctx, node_set_class(node, to_std_string(ctx, argv[0]), true));
    }
    return JS_UNDEFINED;
}

JSValue panel_remove_class(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    if (node != nullptr && argc > 0)
    {
        mark_dirty_if(ctx, node_set_class(node, to_std_string(ctx, argv[0]), false));
    }
    return JS_UNDEFINED;
}

JSValue panel_toggle_class(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    if (node != nullptr && argc > 0)
    {
        const std::string name = to_std_string(ctx, argv[0]);
        mark_dirty_if(ctx, node_set_class(node, name, !node_has_class(node, name)));
    }
    return JS_UNDEFINED;
}

// Panorama's TriggerClass briefly adds a class to fire a transition. We have no
// transitions yet, so adding it (and leaving it) reaches the same steady state.
JSValue panel_trigger_class(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return panel_add_class(ctx, this_val, argc, argv);
}

JSValue panel_set_has_class(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    if (node != nullptr && argc > 1)
    {
        mark_dirty_if(ctx, node_set_class(node, to_std_string(ctx, argv[0]), JS_ToBool(ctx, argv[1]) > 0));
    }
    return JS_UNDEFINED;
}

JSValue panel_has_class(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    const bool has = node != nullptr && argc > 0 && node_has_class(node, to_std_string(ctx, argv[0]));
    return JS_NewBool(ctx, has);
}

JSValue panel_find_child(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    if (node == nullptr || argc < 1)
    {
        return JS_NULL;
    }
    std::string id = to_std_string(ctx, argv[0]);
    if (!id.empty() && id.front() == '#') // FindChildInContext passes "#id"
    {
        id.erase(id.begin());
    }
    return wrap(ctx, node->find_by_id(id));
}

JSValue panel_get_child(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    int index = 0;
    if (node == nullptr || argc < 1 || JS_ToInt32(ctx, &index, argv[0]) < 0 || index < 0 ||
        index >= static_cast<int>(node->children.size()))
    {
        return JS_NULL;
    }
    return wrap(ctx, node->children[static_cast<std::size_t>(index)].get());
}

JSValue panel_get_child_count(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    return JS_NewInt32(ctx, node != nullptr ? static_cast<int>(node->children.size()) : 0);
}

JSValue panel_get_parent(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    return node != nullptr ? wrap(ctx, node->parent) : JS_NULL;
}

JSValue panel_children(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    JSValue arr = JS_NewArray(ctx);
    if (node == nullptr)
    {
        return arr;
    }
    std::uint32_t index = 0;
    for (const auto& child : node->children)
    {
        JS_SetPropertyUint32(ctx, arr, index++, wrap(ctx, child.get()));
    }
    return arr;
}

JSValue panel_remove_and_delete_children(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    if (node != nullptr && !node->children.empty())
    {
        node->children.clear();
        mark_dirty(ctx);
    }
    return JS_UNDEFINED;
}

JSValue panel_delete_async(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    if (node == nullptr || node->parent == nullptr)
    {
        return JS_UNDEFINED;
    }
    auto& siblings = node->parent->children;
    const auto it = std::find_if(siblings.begin(), siblings.end(), [&](const std::unique_ptr<PanoramaNode>& child) {
        return child.get() == node;
    });
    if (it != siblings.end())
    {
        siblings.erase(it);
        mark_dirty(ctx);
    }
    return JS_UNDEFINED;
}

JSValue panel_move_child_before(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    PanoramaNode* parent = node_arg(ctx, this_val);
    PanoramaNode* child = argc > 0 ? node_arg(ctx, argv[0]) : nullptr;
    PanoramaNode* before = argc > 1 ? node_arg(ctx, argv[1]) : nullptr;
    if (parent == nullptr || child == nullptr || child == before)
    {
        return JS_UNDEFINED;
    }

    std::unique_ptr<PanoramaNode> owned;
    if (child->parent == parent)
    {
        auto& siblings = parent->children;
        for (auto it = siblings.begin(); it != siblings.end(); ++it)
        {
            if (it->get() == child)
            {
                owned = std::move(*it);
                siblings.erase(it);
                break;
            }
        }
    }
    else if (impl_of(ctx)->document != nullptr)
    {
        detach_child(*impl_of(ctx)->document, *child, &owned);
    }
    if (!owned)
    {
        return JS_UNDEFINED;
    }

    child->parent = parent;
    auto& siblings = parent->children;
    auto insert_at = before != nullptr ? std::find_if(siblings.begin(), siblings.end(), [&](const std::unique_ptr<PanoramaNode>& node) {
        return node.get() == before;
    }) : siblings.end();
    siblings.insert(insert_at, std::move(owned));
    mark_dirty(ctx);
    return JS_UNDEFINED;
}

JSValue panel_move_child_after(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    PanoramaNode* parent = node_arg(ctx, this_val);
    PanoramaNode* child = argc > 0 ? node_arg(ctx, argv[0]) : nullptr;
    PanoramaNode* after = argc > 1 ? node_arg(ctx, argv[1]) : nullptr;
    if (parent == nullptr || child == nullptr || child == after)
    {
        return JS_UNDEFINED;
    }

    std::unique_ptr<PanoramaNode> owned;
    if (child->parent == parent)
    {
        auto& siblings = parent->children;
        for (auto it = siblings.begin(); it != siblings.end(); ++it)
        {
            if (it->get() == child)
            {
                owned = std::move(*it);
                siblings.erase(it);
                break;
            }
        }
    }
    else if (impl_of(ctx)->document != nullptr)
    {
        detach_child(*impl_of(ctx)->document, *child, &owned);
    }
    if (!owned)
    {
        return JS_UNDEFINED;
    }

    child->parent = parent;
    auto& siblings = parent->children;
    auto insert_at = siblings.end();
    if (after != nullptr)
    {
        insert_at = std::find_if(siblings.begin(), siblings.end(), [&](const std::unique_ptr<PanoramaNode>& node) {
            return node.get() == after;
        });
        if (insert_at != siblings.end())
        {
            ++insert_at;
        }
    }
    siblings.insert(insert_at, std::move(owned));
    mark_dirty(ctx);
    return JS_UNDEFINED;
}

JSValue panel_set_parent(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    PanoramaNode* parent = argc > 0 ? node_arg(ctx, argv[0]) : nullptr;
    if (node == nullptr || parent == nullptr || node == parent || impl_of(ctx)->document == nullptr)
    {
        return JS_UNDEFINED;
    }

    std::unique_ptr<PanoramaNode> owned;
    if (!detach_child(*impl_of(ctx)->document, *node, &owned) || !owned)
    {
        return JS_UNDEFINED;
    }
    node->parent = parent;
    parent->children.push_back(std::move(owned));
    mark_dirty(ctx);
    return JS_UNDEFINED;
}

void append_children_with_class(JSContext* ctx, JSValueConst arr, PanoramaNode& node, const std::string& klass, std::uint32_t& index)
{
    if (node.has_class(klass))
    {
        JS_SetPropertyUint32(ctx, arr, index++, wrap(ctx, &node));
    }
    for (const auto& child : node.children)
    {
        append_children_with_class(ctx, arr, *child, klass, index);
    }
}

JSValue panel_find_children_with_class(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    JSValue arr = JS_NewArray(ctx);
    if (node == nullptr || argc < 1)
    {
        return arr;
    }
    std::uint32_t index = 0;
    const std::string klass = to_std_string(ctx, argv[0]);
    for (const auto& child : node->children)
    {
        append_children_with_class(ctx, arr, *child, klass, index);
    }
    return arr;
}

JSValue panel_data(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    if (node == nullptr)
    {
        return JS_NewObject(ctx);
    }
    PanoramaRuntime::Impl* impl = impl_of(ctx);
    auto it = impl->panel_data.find(node);
    if (it == impl->panel_data.end())
    {
        it = impl->panel_data.emplace(node, JS_NewObject(ctx)).first;
    }
    return JS_DupValue(ctx, it->second);
}

JSValue panel_set_attribute(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    if (node != nullptr && argc > 1)
    {
        const std::string key = to_std_string(ctx, argv[0]);
        const bool changed = set_node_attribute(*node, key, to_std_string(ctx, argv[1]));
        if (changed && key == "placeholder")
        {
            ensure_panorama_text_entry_placeholders(*node, [ctx](std::string_view text) {
                return impl_of(ctx)->localization.localize(text);
            });
        }
        mark_dirty_if(ctx, changed);
    }
    return JS_UNDEFINED;
}

JSValue panel_get_attribute(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    const std::string fallback = argc > 1 ? to_std_string(ctx, argv[1]) : std::string();
    if (node == nullptr || argc < 1)
    {
        return JS_NewString(ctx, fallback.c_str());
    }
    return JS_NewString(ctx, node_attribute(node, to_std_string(ctx, argv[0]), fallback).c_str());
}

JSValue panel_set_image(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    if (node != nullptr && argc > 0)
    {
        mark_dirty_if(ctx, set_node_attribute(*node, "src", to_std_string(ctx, argv[0])));
    }
    return JS_UNDEFINED;
}

JSValue panel_set_movie(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    if (node != nullptr && argc > 0)
    {
        mark_dirty_if(ctx, set_node_attribute(*node, "src", to_std_string(ctx, argv[0])));
    }
    return JS_UNDEFINED;
}

JSValue panel_set_sound(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    if (node != nullptr && argc > 0)
    {
        mark_dirty_if(ctx, set_node_attribute(*node, "sound", to_std_string(ctx, argv[0])));
    }
    return JS_UNDEFINED;
}

JSValue panel_play(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    if (node != nullptr)
    {
        bool changed = set_node_attribute(*node, "playing", "true");
        changed = erase_node_attribute(*node, "paused") || changed;
        mark_dirty_if(ctx, changed);
    }
    return JS_UNDEFINED;
}

JSValue panel_pause(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    if (node != nullptr)
    {
        const bool paused = argc <= 0 || JS_ToBool(ctx, argv[0]) != 0;
        bool changed = set_node_attribute(*node, "paused", paused ? "true" : "false");
        changed = set_node_attribute(*node, "playing", paused ? "false" : "true") || changed;
        mark_dirty_if(ctx, changed);
    }
    return JS_UNDEFINED;
}

JSValue panel_switch_class(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    if (node == nullptr || argc < 2)
    {
        return JS_UNDEFINED;
    }
    PanoramaRuntime::Impl* impl = impl_of(ctx);
    const std::string slot = to_std_string(ctx, argv[0]);
    const std::string klass = to_std_string(ctx, argv[1]);
    std::string& current = impl->switch_classes[node][slot];
    if (current == klass)
    {
        return JS_UNDEFINED;
    }
    bool changed = false;
    if (!current.empty())
    {
        changed = node_set_class(node, current, false) || changed;
    }
    current = klass;
    if (!current.empty())
    {
        changed = node_set_class(node, current, true) || changed;
    }
    mark_dirty_if(ctx, changed);
    return JS_UNDEFINED;
}

JSValue panel_set_panel_event(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    if (node == nullptr || argc < 2)
    {
        return JS_UNDEFINED;
    }
    PanoramaRuntime::Impl* impl = impl_of(ctx);
    const std::string event_name = to_std_string(ctx, argv[0]);
    if (event_name.empty())
    {
        return JS_UNDEFINED;
    }

    auto& by_event = impl->panel_events[node];
    auto old = by_event.find(event_name);
    if (old != by_event.end())
    {
        JS_FreeValue(ctx, old->second);
        by_event.erase(old);
    }
    if (JS_IsFunction(ctx, argv[1]))
    {
        by_event.emplace(event_name, JS_DupValue(ctx, argv[1]));
        node->attributes[event_name] = kPanelEventMarker;
    }
    mark_dirty(ctx);
    return JS_UNDEFINED;
}

JSValue panel_set_ready_for_display(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
    const bool ready = argc <= 0 || JS_ToBool(ctx, argv[0]) > 0;
    impl_of(ctx)->fire(ready ? "ReadyForDisplay" : "UnreadyForDisplay", 0, nullptr);
    return JS_UNDEFINED;
}

JSValue panel_b_is_transparent(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    const bool transparent = node == nullptr || node->visibility_override == 0 || !node->computed.visible || node->computed.opacity <= 0.001F;
    return JS_NewBool(ctx, transparent);
}

JSValue panel_is_valid(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
    return JS_NewBool(ctx, node_arg(ctx, this_val) != nullptr);
}

// --- overflow:scroll panel APIs ----------------------------------------------
// The setters store a clamped offset; the next layout pass applies it to the
// children (mark_dirty_if drives the host's recompute+relayout).

JSValue panel_scroll_to_top(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    if (node != nullptr)
    {
        panorama_cancel_scroll_animation(*node);
        mark_dirty_if(ctx, panorama_set_scroll_offset(*node, node->scroll_offset_x, 0.0F));
    }
    return JS_UNDEFINED;
}

JSValue panel_scroll_to_bottom(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    if (node != nullptr)
    {
        panorama_cancel_scroll_animation(*node);
        mark_dirty_if(ctx, panorama_set_scroll_offset(*node, node->scroll_offset_x, node->max_scroll_y));
    }
    return JS_UNDEFINED;
}

// ScrollParentToMakePanelFit( nFitMode, bImmediate ): CS:GO's settings nav uses
// it to bring section headers into view. The scroll resolves as WebCore
// scrollRectToVisible with alignToEdgeIfNeeded (visible: stay put; partial/
// hidden: align to the closest edge); bImmediate=false glides there via the
// WebCore smooth-scroll spring, bImmediate=true jumps.
JSValue panel_scroll_parent_to_make_panel_fit(
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    if (node != nullptr)
    {
        const bool immediate = argc >= 2 && JS_ToBool(ctx, argv[1]) != 0;
        mark_dirty_if(ctx, panorama_scroll_ancestors_to_fit(*node, /*smooth=*/!immediate));
    }
    return JS_UNDEFINED;
}

JSValue panel_get_scroll_offset_x(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    return JS_NewFloat64(ctx, node != nullptr ? node->scroll_offset_x : 0.0F);
}

JSValue panel_get_scroll_offset_y(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    return JS_NewFloat64(ctx, node != nullptr ? node->scroll_offset_y : 0.0F);
}

JSValue panel_is_selected(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    return JS_NewBool(ctx, node != nullptr && (node->selected || node_has_class(node, "checked")));
}

PanoramaNode* direct_child_by_id(PanoramaNode* node, const std::string& id)
{
    if (node == nullptr)
    {
        return nullptr;
    }
    for (const auto& child : node->children)
    {
        if (child->id == id)
        {
            return child.get();
        }
    }
    return nullptr;
}

bool set_dropdown_selected(PanoramaNode* node, PanoramaNode* option)
{
    if (node == nullptr)
    {
        return false;
    }
    bool changed = option != nullptr && !option->id.empty()
        ? set_node_attribute(*node, "selected", option->id)
        : erase_node_attribute(*node, "selected");
    for (const auto& child : node->children)
    {
        const bool selected = child.get() == option;
        if (child->selected != selected)
        {
            child->selected = selected;
            changed = true;
        }
        changed = node_set_class(child.get(), "checked", selected) || changed;
    }
    return changed;
}

bool set_dropdown_selected_by_id(PanoramaNode* node, const std::string& id)
{
    return set_dropdown_selected(node, direct_child_by_id(node, id));
}

JSValue panel_set_selected_method(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    if (node == nullptr || argc < 1)
    {
        return JS_UNDEFINED;
    }
    const std::string id = to_std_string(ctx, argv[0]);
    if (direct_child_by_id(node, id) != nullptr)
    {
        mark_dirty_if(ctx, set_dropdown_selected_by_id(node, id));
    }
    else
    {
        set_selected_state(ctx, node, JS_ToBool(ctx, argv[0]) > 0);
    }
    return JS_UNDEFINED;
}

JSValue panel_get_selected(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    if (node == nullptr)
    {
        return JS_NULL;
    }
    const auto selected_id = node->attributes.find("selected");
    if (selected_id != node->attributes.end())
    {
        if (PanoramaNode* selected = direct_child_by_id(node, selected_id->second))
        {
            return wrap(ctx, selected);
        }
    }
    for (const auto& child : node->children)
    {
        if (child->selected || node_has_class(child.get(), "checked"))
        {
            return wrap(ctx, child.get());
        }
    }
    return !node->children.empty() ? wrap(ctx, node->children.front().get()) : JS_NULL;
}

JSValue panel_add_option(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    PanoramaNode* option = argc > 0 ? node_arg(ctx, argv[0]) : nullptr;
    if (node == nullptr || option == nullptr)
    {
        return JS_UNDEFINED;
    }
    if (option->parent != node && impl_of(ctx)->document != nullptr)
    {
        std::unique_ptr<PanoramaNode> owned;
        if (detach_child(*impl_of(ctx)->document, *option, &owned) && owned)
        {
            option->parent = node;
            node->children.push_back(std::move(owned));
        }
    }
    mark_dirty(ctx);
    return JS_UNDEFINED;
}

JSValue panel_remove_all_options(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    if (node != nullptr)
    {
        node->children.clear();
        node->attributes.erase("selected");
        mark_dirty(ctx);
    }
    return JS_UNDEFINED;
}

// We model dropdown options as direct children of the dropdown, so the control's
// internal "menu" panel is the dropdown itself: AccessDropDownMenu().Children()
// returns the options (the real CS:GO menu uses exactly that to enumerate them).
JSValue panel_access_dropdown_menu(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
    return JS_DupValue(ctx, this_val);
}

JSValue panel_set_selected_index(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    if (node == nullptr || argc < 1)
    {
        return JS_UNDEFINED;
    }
    std::int32_t index = 0;
    JS_ToInt32(ctx, &index, argv[0]);
    if (index >= 0 && static_cast<std::size_t>(index) < node->children.size())
    {
        PanoramaNode* selected = node->children[static_cast<std::size_t>(index)].get();
        mark_dirty_if(ctx, set_dropdown_selected(node, selected));
    }
    return JS_UNDEFINED;
}

JSValue panel_set_dialog_variable(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    if (node == nullptr || argc < 2)
    {
        return JS_UNDEFINED;
    }
    PanoramaRuntime::Impl* impl = impl_of(ctx);
    const std::string key = to_std_string(ctx, argv[0]);
    const std::string value = to_std_string(ctx, argv[1]);
    auto& vars = impl->dialog_vars[node];
    const auto old = vars.find(key);
    if (old != vars.end() && old->second == value)
    {
        return JS_UNDEFINED;
    }
    vars[key] = value;
    const std::string old_text = node->text;
    impl->apply_dialog_variables(node);
    mark_dirty_if(ctx, old_text != node->text);
    return JS_UNDEFINED;
}

JSValue panel_noop(JSContext* /*ctx*/, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/)
{
    return JS_UNDEFINED;
}

// BLoadLayout / LoadLayout / LoadLayoutAsync: load a layout XML file into this
// panel via the host loader (parses, merges styles, runs the sublayout's scripts).
JSValue panel_load_layout(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    PanoramaRuntime::Impl* impl = impl_of(ctx);
    if (node != nullptr && argc > 0 && impl->load_layout_file)
    {
        impl->load_layout_file(*node, to_std_string(ctx, argv[0]));
        impl->dirty = true;
    }
    return JS_NewBool(ctx, 1);
}

// BLoadLayoutSnippet / BCreateChildren: instantiate a named <snippet> here.
JSValue panel_load_snippet(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    PanoramaRuntime::Impl* impl = impl_of(ctx);
    if (node != nullptr && argc > 0 && impl->load_layout_snippet)
    {
        impl->load_layout_snippet(*node, to_std_string(ctx, argv[0]));
        impl->dirty = true;
    }
    return JS_NewBool(ctx, 1);
}

JSValue panel_has_layout_snippet(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
    PanoramaRuntime::Impl* impl = impl_of(ctx);
    if (argc <= 0 || !impl->has_layout_snippet)
    {
        return JS_NewBool(ctx, 0);
    }
    return JS_NewBool(ctx, impl->has_layout_snippet(to_std_string(ctx, argv[0])));
}

//--- Panel accessors ---------------------------------------------------------

JSValue panel_get_id(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    return JS_NewString(ctx, node != nullptr ? node->id.c_str() : "");
}

JSValue panel_get_visible(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    if (node == nullptr)
    {
        return JS_NewBool(ctx, 0);
    }
    const bool visible = node->visibility_override != 0 && node->computed.visible;
    return JS_NewBool(ctx, visible);
}

JSValue panel_set_visible(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    if (node != nullptr && argc > 0)
    {
        // A node-level override applied after the cascade, so it survives style
        // recompute (CS:GO has no generic `.hidden` rule to lean on).
        const int visibility = JS_ToBool(ctx, argv[0]) > 0 ? 1 : 0;
        if (node->visibility_override != visibility)
        {
            node->visibility_override = visibility;
            mark_dirty(ctx);
        }
    }
    return JS_UNDEFINED;
}

JSValue panel_get_enabled(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    const bool enabled = node != nullptr && node_attribute(node, "enabled", "true") != "false";
    return JS_NewBool(ctx, enabled);
}

JSValue panel_set_enabled(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    if (node != nullptr && argc > 0)
    {
        const bool enabled = JS_ToBool(ctx, argv[0]) > 0;
        bool changed = set_node_attribute(*node, "enabled", enabled ? "true" : "false");
        changed = node_set_class(node, "disabled", !enabled) || changed;
        mark_dirty_if(ctx, changed);
    }
    return JS_UNDEFINED;
}

JSValue panel_get_checked(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    return JS_NewBool(ctx, node != nullptr && (node->selected || node_has_class(node, "checked")));
}

JSValue panel_set_checked(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    if (node != nullptr && argc > 0)
    {
        set_selected_state(ctx, node, JS_ToBool(ctx, argv[0]) > 0);
    }
    return JS_UNDEFINED;
}

JSValue panel_get_selected_prop(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return panel_get_checked(ctx, this_val, argc, argv);
}

JSValue panel_set_selected_prop(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return panel_set_checked(ctx, this_val, argc, argv);
}

JSValue panel_get_group(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    return JS_NewString(ctx, node_attribute(node, "group", "").c_str());
}

JSValue panel_set_group(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    if (node != nullptr && argc > 0)
    {
        mark_dirty_if(ctx, set_node_attribute(*node, "group", to_std_string(ctx, argv[0])));
    }
    return JS_UNDEFINED;
}

JSValue panel_get_tooltip(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    return JS_NewString(ctx, node_attribute(node, "tooltip", "").c_str());
}

JSValue panel_set_tooltip(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    if (node != nullptr && argc > 0)
    {
        mark_dirty_if(ctx, set_node_attribute(*node, "tooltip", to_std_string(ctx, argv[0])));
    }
    return JS_UNDEFINED;
}

JSValue panel_get_marked_for_delete(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    return JS_NewBool(ctx, node_attribute(node, "marked_for_delete", "false") == "true");
}

JSValue panel_set_marked_for_delete(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    if (node != nullptr && argc > 0)
    {
        mark_dirty_if(ctx, set_node_attribute(*node, "marked_for_delete", JS_ToBool(ctx, argv[0]) > 0 ? "true" : "false"));
    }
    return JS_UNDEFINED;
}

JSValue panel_get_string_data(JSContext* ctx, JSValueConst this_val, const char* key)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    return JS_NewString(ctx, node_attribute(node, key, "").c_str());
}

JSValue panel_set_string_data(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, const char* key)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    if (node != nullptr && argc > 0)
    {
        mark_dirty_if(ctx, set_node_attribute(*node, key, to_std_string(ctx, argv[0])));
    }
    return JS_UNDEFINED;
}

JSValue panel_get_steamid(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) { return panel_get_string_data(ctx, this_val, "steamid"); }
JSValue panel_set_steamid(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) { return panel_set_string_data(ctx, this_val, argc, argv, "steamid"); }
JSValue panel_get_xuid(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) { return panel_get_string_data(ctx, this_val, "xuid"); }
JSValue panel_set_xuid(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) { return panel_set_string_data(ctx, this_val, argc, argv, "xuid"); }

JSValue panel_get_text(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    return JS_NewString(ctx, node != nullptr ? node->text.c_str() : "");
}

JSValue panel_set_text(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    if (node != nullptr && argc > 0)
    {
        const std::string text = impl_of(ctx)->localization.localize(to_std_string(ctx, argv[0]));
        const bool text_changed = node->text != text;
        if (text_changed)
        {
            node->text = text;
        }
        const bool input_class_before = node_has_class(node, "TextEntryHasInput");
        sync_panorama_text_entry_input_class(*node);
        mark_dirty_if(ctx, text_changed || input_class_before != node_has_class(node, "TextEntryHasInput"));
    }
    return JS_UNDEFINED;
}

JSValue panel_get_style(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/)
{
    PanoramaNode* node = node_arg(ctx, this_val);
    if (node == nullptr)
    {
        return JS_NULL;
    }
    JSValue obj = JS_NewObjectClass(ctx, impl_of(ctx)->style_class_id);
    if (JS_IsException(obj))
    {
        return obj;
    }
    JS_DefinePropertyValueStr(ctx, obj, kStylePanelSlot, JS_DupValue(ctx, this_val), 0);
    return obj;
}

JSValue style_get_empty(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/)
{
    return JS_NewString(ctx, "");
}

JSValue style_set_property(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, const char* property)
{
    PanoramaNode* node = style_node_arg(ctx, this_val);
    if (node != nullptr && argc > 0)
    {
        mark_dirty_if(ctx, set_inline_style_property(*node, property, to_std_string(ctx, argv[0])));
    }
    return JS_UNDEFINED;
}

JSValue style_set_background_image(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return style_set_property(ctx, this_val, argc, argv, "background-image");
}

JSValue style_set_background_position(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return style_set_property(ctx, this_val, argc, argv, "background-position");
}

JSValue style_set_background_size(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return style_set_property(ctx, this_val, argc, argv, "background-size");
}

JSValue style_set_width(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return style_set_property(ctx, this_val, argc, argv, "width");
}

JSValue style_set_height(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return style_set_property(ctx, this_val, argc, argv, "height");
}

JSValue style_set_opacity(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return style_set_property(ctx, this_val, argc, argv, "opacity");
}

JSValue style_set_transform(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    return style_set_property(ctx, this_val, argc, argv, "transform");
}

//--- $ globals ---------------------------------------------------------------

// `$` is callable in Panorama: `$('#id')` resolves a panel by id, searching the
// active context panel first and then the whole document.
JSValue dollar_select(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
    if (argc < 1)
    {
        return JS_NULL;
    }
    PanoramaRuntime::Impl* impl = impl_of(ctx);
    std::string selector = to_std_string(ctx, argv[0]);
    if (!selector.empty() && selector.front() == '#')
    {
        selector.erase(selector.begin());
    }

    PanoramaNode* base = impl->current_context();
    PanoramaNode* found = base != nullptr ? base->find_by_id(selector) : nullptr;
    if (found == nullptr && impl->document != nullptr)
    {
        found = impl->document->find_by_id(selector);
    }
    return wrap(ctx, found);
}

// $.__host(action, arg): bridge JS to host engine actions (console command, match
// launch). Wired to GameInterfaceAPI.ConsoleCommand / LobbyAPI.StartMatchmaking in
// the prelude.
JSValue dollar_host(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
    PanoramaRuntime::Impl* impl = impl_of(ctx);
    if (argc >= 1)
    {
        const std::string action = to_std_string(ctx, argv[0]);
        const std::string arg = argc >= 2 ? to_std_string(ctx, argv[1]) : std::string();
        if (impl->host_action)
        {
            impl->host_action(action, arg);
        }
        else if (impl->client != nullptr)
        {
            impl->client->on_host_action(action, arg);
        }
    }
    return JS_UNDEFINED;
}

JSValue dollar_msg(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
    std::string message;
    for (int i = 0; i < argc; ++i)
    {
        if (i > 0)
        {
            message += ' ';
        }
        message += to_std_string(ctx, argv[i]);
    }
    pano_log_info("[panorama] {}", message);
    return JS_UNDEFINED;
}

JSValue dollar_warning(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
    std::string message;
    for (int i = 0; i < argc; ++i)
    {
        if (i > 0)
        {
            message += ' ';
        }
        message += to_std_string(ctx, argv[i]);
    }
    pano_log_warning("[panorama] {}", message);
    return JS_UNDEFINED;
}

JSValue dollar_localize(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
    if (argc < 1)
    {
        return JS_NewString(ctx, "");
    }
    const std::string localized = impl_of(ctx)->localization.localize(to_std_string(ctx, argv[0]));
    return JS_NewString(ctx, localized.c_str());
}

JSValue dollar_get_context_panel(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/)
{
    return wrap(ctx, impl_of(ctx)->current_context());
}

JSValue dollar_create_panel(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
    if (argc < 2)
    {
        return JS_NULL;
    }
    PanoramaNode* parent = node_arg(ctx, argv[1]);
    if (parent == nullptr)
    {
        return JS_NULL;
    }
    PanoramaRuntime::Impl* impl = impl_of(ctx);

    std::string type = to_std_string(ctx, argv[0]);
    std::string lowered = type;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    auto created = std::make_unique<PanoramaNode>();
    PanoramaNode* raw = created.get();
    raw->tag = std::move(type);
    raw->tag_lower = std::move(lowered);
    raw->parent = parent;
    if (argc > 2 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2]))
    {
        raw->id = to_std_string(ctx, argv[2]);
    }
    if (argc > 3 && JS_IsObject(argv[3]))
    {
        JSValue class_value = JS_GetPropertyStr(ctx, argv[3], "class");
        if (!JS_IsUndefined(class_value))
        {
            std::istringstream stream(to_std_string(ctx, class_value));
            std::string klass;
            while (stream >> klass)
            {
                raw->classes.push_back(klass);
            }
        }
        JS_FreeValue(ctx, class_value);

        JSValue data_value = JS_GetPropertyStr(ctx, argv[3], "data");
        if (!JS_IsUndefined(data_value))
        {
            raw->attributes["data"] = to_std_string(ctx, data_value);
        }
        JS_FreeValue(ctx, data_value);

        JSValue style_value = JS_GetPropertyStr(ctx, argv[3], "style");
        if (!JS_IsUndefined(style_value))
        {
            raw->inline_style = to_std_string(ctx, style_value);
        }
        JS_FreeValue(ctx, style_value);

        JSValue text_value = JS_GetPropertyStr(ctx, argv[3], "text");
        if (!JS_IsUndefined(text_value))
        {
            raw->text = to_std_string(ctx, text_value);
        }
        JS_FreeValue(ctx, text_value);

        for (const char* key : {
                 "src",
                 "defaultsrc",
                 "texturewidth",
                 "textureheight",
                 "html",
                 "hittest",
                 "data-type",
                 "mapname",
                 "placeholder",
                 "tooltip",
                 "onactivate",
             })
        {
            JSValue value = JS_GetPropertyStr(ctx, argv[3], key);
            if (!JS_IsUndefined(value) && !JS_IsNull(value))
            {
                raw->attributes[key] = to_std_string(ctx, value);
            }
            JS_FreeValue(ctx, value);
        }
    }
    ensure_panorama_text_entry_placeholders(*raw, [impl](std::string_view text) {
        return impl->localization.localize(text);
    });
    parent->children.push_back(std::move(created));
    impl->dirty = true;
    return wrap(ctx, raw);
}

JSValue dollar_register_event_handler(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
    // Handles RegisterEventHandler(event, panel, fn) and
    // RegisterForUnhandledEvent(event, fn): the handler is always the last arg.
    if (argc < 2)
    {
        return JS_NewInt32(ctx, 0);
    }
    PanoramaRuntime::Impl* impl = impl_of(ctx);
    const std::string name = to_std_string(ctx, argv[0]);
    JSValueConst fn = argv[argc - 1];
    if (!JS_IsFunction(ctx, fn))
    {
        return JS_NewInt32(ctx, 0);
    }
    // 3-arg form RegisterEventHandler(event, panel, fn): the handler is scoped to
    // that panel — source-targeted fires (PropertyTransitionEnd) skip it for
    // other panels' events.
    PanoramaNode* target = argc >= 3 ? impl->panel_node(argv[1]) : nullptr;
    impl->event_handlers[name].push_back(
        PanoramaRuntime::Impl::EventHandler{JS_DupValue(ctx, fn), impl->innermost_context(), target});
    return JS_NewInt32(ctx, static_cast<int>(impl->event_handlers[name].size()));
}

JSValue dollar_dispatch_event(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
    if (argc >= 1)
    {
        impl_of(ctx)->fire(to_std_string(ctx, argv[0]), argc - 1, argv + 1);
    }
    return JS_UNDEFINED;
}

JSValue dollar_schedule(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
    if (argc < 2 || !JS_IsFunction(ctx, argv[1]))
    {
        return JS_NewInt32(ctx, 0);
    }
    PanoramaRuntime::Impl* impl = impl_of(ctx);
    double seconds = 0.0;
    JS_ToFloat64(ctx, &seconds, argv[0]);

    PanoramaRuntime::Impl::Scheduled entry;
    entry.id = impl->next_schedule_id++;
    entry.remaining = seconds;
    entry.fn = JS_DupValue(ctx, argv[1]);
    entry.context = impl->innermost_context();
    impl->scheduled.push_back(entry);
    return JS_NewInt32(ctx, entry.id);
}

JSValue dollar_cancel_scheduled(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv)
{
    if (argc < 1)
    {
        return JS_UNDEFINED;
    }
    int id = 0;
    JS_ToInt32(ctx, &id, argv[0]);
    PanoramaRuntime::Impl* impl = impl_of(ctx);
    for (auto it = impl->scheduled.begin(); it != impl->scheduled.end(); ++it)
    {
        if (it->id == id)
        {
            JS_FreeValue(ctx, it->fn);
            impl->scheduled.erase(it);
            break;
        }
    }
    return JS_UNDEFINED;
}

void define_method(JSContext* ctx, JSValueConst obj, const char* name, JSCFunction* fn, int length)
{
    JS_SetPropertyStr(ctx, obj, name, JS_NewCFunction(ctx, fn, name, length));
}

void define_accessor(JSContext* ctx, JSValueConst obj, const char* name, JSCFunction* getter, JSCFunction* setter)
{
    JSAtom atom = JS_NewAtom(ctx, name);
    JSValue get_fn = JS_NewCFunction(ctx, getter, name, 0);
    JSValue set_fn = setter != nullptr ? JS_NewCFunction(ctx, setter, name, 1) : JS_UNDEFINED;
    JS_DefinePropertyGetSet(ctx, obj, atom, get_fn, set_fn, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, atom);
}
}

//=============================================================================
// Impl methods
//=============================================================================

JSValue PanoramaRuntime::Impl::wrap_panel(PanoramaNode* node)
{
    if (node == nullptr)
    {
        return JS_NULL;
    }
    // One wrapper per node: script-side identity (`a === b` for the same panel)
    // and a single place to neutralize when the node is destroyed.
    const auto it = panel_wrappers.find(node);
    if (it != panel_wrappers.end())
    {
        return JS_DupValue(ctx, it->second);
    }
    JSValue obj = JS_NewObjectClass(ctx, panel_class_id);
    if (JS_IsException(obj))
    {
        return obj;
    }
    JS_SetOpaque(obj, node);
    panel_wrappers.emplace(node, JS_DupValue(ctx, obj));
    return obj;
}

PanoramaNode* PanoramaRuntime::Impl::panel_node(JSValueConst value) const
{
    return static_cast<PanoramaNode*>(JS_GetOpaque(value, panel_class_id));
}

void PanoramaRuntime::Impl::on_panorama_node_destroyed(PanoramaNode& node)
{
    if (ctx == nullptr)
    {
        return;
    }

    // Neutralize the JS wrapper: surviving script references keep a valid JS
    // object whose opaque is null, which every panel method treats as "panel
    // gone" (and IsValid() reports false).
    const auto wrapper = panel_wrappers.find(&node);
    if (wrapper != panel_wrappers.end())
    {
        JS_SetOpaque(wrapper->second, nullptr);
        JS_FreeValue(ctx, wrapper->second);
        panel_wrappers.erase(wrapper);
    }

    const auto data = panel_data.find(&node);
    if (data != panel_data.end())
    {
        JS_FreeValue(ctx, data->second);
        panel_data.erase(data);
    }

    const auto events = panel_events.find(&node);
    if (events != panel_events.end())
    {
        for (auto& [event_name, fn] : events->second)
        {
            JS_FreeValue(ctx, fn);
        }
        panel_events.erase(events);
    }

    switch_classes.erase(&node);
    dialog_templates.erase(&node);
    dialog_vars.erase(&node);

    // Handlers registered by the dying panel's script context — or scoped TO the
    // dying panel via RegisterEventHandler(event, panel, fn) — die with it (real
    // Panorama scopes a panel's registrations to the panel); in-flight dispatch
    // batches hold their own dup'd functions, so only their pointers are nulled.
    for (auto& [event_name, handlers] : event_handlers)
    {
        handlers.erase(
            std::remove_if(
                handlers.begin(),
                handlers.end(),
                [&](EventHandler& handler) {
                    if (handler.context != &node && handler.target != &node)
                    {
                        return false;
                    }
                    JS_FreeValue(ctx, handler.fn);
                    return true;
                }),
            handlers.end());
    }
    for (std::vector<EventHandler>* batch : live_handler_batches)
    {
        for (EventHandler& handler : *batch)
        {
            if (handler.context == &node)
            {
                handler.context = nullptr;
            }
            if (handler.target == &node)
            {
                handler.target = nullptr;
            }
        }
    }

    // Scheduled callbacks survive (they are script-owned, not panel-owned) but
    // fall back to the document context.
    for (Scheduled& entry : scheduled)
    {
        if (entry.context == &node)
        {
            entry.context = nullptr;
        }
    }
    if (firing_scheduled != nullptr)
    {
        for (Scheduled& entry : *firing_scheduled)
        {
            if (entry.context == &node)
            {
                entry.context = nullptr;
            }
        }
    }

    for (PanoramaNode*& entry : context_stack)
    {
        if (entry == &node)
        {
            entry = nullptr;
        }
    }
    if (document == &node)
    {
        document = nullptr;
    }
}

void PanoramaRuntime::Impl::dump_pending_error(const char* origin)
{
    JSValue exception = JS_GetException(ctx);
    std::string message = to_std_string(ctx, exception);

    JSValue stack = JS_GetPropertyStr(ctx, exception, "stack");
    if (!JS_IsUndefined(stack))
    {
        const std::string stack_text = to_std_string(ctx, stack);
        if (!stack_text.empty())
        {
            message += "\n" + stack_text;
        }
    }
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, exception);

    pano_log_warning("[panorama] {} error: {}", origin, message);
}

void PanoramaRuntime::Impl::fire(const std::string& name, int argc, JSValueConst* argv, PanoramaNode* source)
{
    const auto it = event_handlers.find(name);
    if (it == event_handlers.end())
    {
        return;
    }

    // Copy so handlers may (un)register during dispatch without invalidating us.
    // Functions are dup'd (a handler that deletes a panel removes that panel's
    // registrations and frees the registry's references — our copies must hold
    // their own), and the batch is registered so node destruction can null dead
    // contexts inside it. Panel-scoped handlers are filtered here, against the
    // event's source panel; null-source fires keep delivering to everyone.
    std::vector<EventHandler> handlers;
    handlers.reserve(it->second.size());
    for (const EventHandler& handler : it->second)
    {
        if (source != nullptr && handler.target != nullptr && handler.target != source)
        {
            continue;
        }
        handlers.push_back(EventHandler{JS_DupValue(ctx, handler.fn), handler.context, handler.target});
    }
    live_handler_batches.push_back(&handlers);

    for (const EventHandler& handler : handlers)
    {
        // Each handler resolves $.GetContextPanel() against the layout root of
        // the script that registered it (captured at registration).
        ContextScope scope(*this, handler.context);
        JSValue result = JS_Call(ctx, handler.fn, JS_UNDEFINED, argc, argv);
        if (JS_IsException(result))
        {
            dump_pending_error(("event '" + name + "'").c_str());
        }
        JS_FreeValue(ctx, result);
    }

    live_handler_batches.pop_back();
    for (const EventHandler& handler : handlers)
    {
        JS_FreeValue(ctx, handler.fn);
    }
}

void PanoramaRuntime::Impl::run_code(PanoramaNode* context, const std::string& code, const char* origin)
{
    ContextScope scope(*this, context);

    JSValue result = JS_Eval(ctx, code.c_str(), code.size(), origin, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result))
    {
        dump_pending_error(origin);
    }
    JS_FreeValue(ctx, result);
}

void PanoramaRuntime::Impl::apply_dialog_variables(PanoramaNode* node)
{
    auto template_it = dialog_templates.find(node);
    if (template_it == dialog_templates.end())
    {
        template_it = dialog_templates.emplace(node, node->text).first;
    }

    std::string expanded = template_it->second;
    const auto vars_it = dialog_vars.find(node);
    if (vars_it != dialog_vars.end())
    {
        for (const auto& [key, value] : vars_it->second)
        {
            for (const char* prefix : {"s:", "d:", "i:", "f:", "g:"})
            {
                const std::string token = "{" + std::string(prefix) + key + "}";
                std::size_t pos = 0;
                while ((pos = expanded.find(token, pos)) != std::string::npos)
                {
                    expanded.replace(pos, token.size(), value);
                    pos += value.size();
                }
            }
        }
    }

    node->text = expanded;
}

//=============================================================================
// PanoramaRuntime
//=============================================================================

PanoramaRuntime::PanoramaRuntime() = default;

PanoramaRuntime::~PanoramaRuntime()
{
    shutdown();
}

PanoramaRuntimeScript make_panorama_csgo_bootstrap_script()
{
    return PanoramaRuntimeScript{
        .origin = "panorama://csgo-host-api",
        .source = make_panorama_csgo_host_prelude_source(),
    };
}

bool PanoramaRuntime::active() const noexcept
{
    return impl_ != nullptr && impl_->ctx != nullptr;
}

bool PanoramaRuntime::consume_dirty()
{
    if (impl_ == nullptr || !impl_->dirty)
    {
        return false;
    }
    impl_->dirty = false;
    return true;
}

bool PanoramaRuntime::initialize(
    PanoramaNode& root,
    const PanoramaPackage& package,
    const std::vector<std::string>& scripts,
    const std::filesystem::path& resource_root)
{
    PanoramaResourceManager resources;
    resources.add_provider(std::make_unique<PanoramaPackageResourceProvider>(package), 0, "package");
    if (!resource_root.empty())
    {
        resources.add_provider(std::make_unique<PanoramaDirectoryResourceProvider>(resource_root), 100, "resource-root");
    }
    return initialize(root, resources, scripts, resource_root);
}

bool PanoramaRuntime::initialize(
    PanoramaNode& root,
    const PanoramaResourceManager& resources,
    const std::vector<std::string>& scripts,
    const std::filesystem::path& resource_root)
{
    std::vector<PanoramaRuntimeScriptInclude> includes;
    includes.reserve(scripts.size());
    for (const std::string& path : scripts)
    {
        includes.push_back(PanoramaRuntimeScriptInclude{path, nullptr});
    }
    return initialize_with_script_contexts(root, resources, includes, resource_root);
}

bool PanoramaRuntime::initialize_with_script_contexts(
    PanoramaNode& root,
    const PanoramaResourceManager& resources,
    const std::vector<PanoramaRuntimeScriptInclude>& scripts,
    const std::filesystem::path& resource_root)
{
    shutdown();

    auto impl = std::make_unique<Impl>();
    impl->rt = JS_NewRuntime();
    if (impl->rt == nullptr)
    {
        pano_log_warning("[panorama] failed to create QuickJS runtime");
        return false;
    }
    impl->ctx = JS_NewContext(impl->rt);
    if (impl->ctx == nullptr)
    {
        JS_FreeRuntime(impl->rt);
        pano_log_warning("[panorama] failed to create QuickJS context");
        return false;
    }

    JSContext* ctx = impl->ctx;
    JS_SetContextOpaque(ctx, impl.get());
    impl->document = &root;
    panorama_add_node_lifetime_observer(*impl);
    impl->load_layout_file = file_loader_;       // so init-time BLoadLayout works
    impl->load_layout_snippet = snippet_loader_;
    impl->has_layout_snippet = snippet_exists_;
    impl->host_action = host_action_;
    impl->client = client_;
    impl->bootstrap_scripts = bootstrap_scripts_;
    impl->localization.load(resource_root);

    // Register the Panel class and its prototype.
    JS_NewClassID(impl->rt, &impl->panel_class_id);
    JSClassDef panel_def{};
    panel_def.class_name = "Panel";
    JS_NewClass(impl->rt, impl->panel_class_id, &panel_def);

    JS_NewClassID(impl->rt, &impl->style_class_id);
    JSClassDef style_def{};
    style_def.class_name = "PanelStyle";
    JS_NewClass(impl->rt, impl->style_class_id, &style_def);
    JSValue style_proto = JS_NewObject(ctx);
    define_accessor(ctx, style_proto, "backgroundImage", style_get_empty, style_set_background_image);
    define_accessor(ctx, style_proto, "backgroundPosition", style_get_empty, style_set_background_position);
    define_accessor(ctx, style_proto, "backgroundSize", style_get_empty, style_set_background_size);
    define_accessor(ctx, style_proto, "width", style_get_empty, style_set_width);
    define_accessor(ctx, style_proto, "height", style_get_empty, style_set_height);
    define_accessor(ctx, style_proto, "opacity", style_get_empty, style_set_opacity);
    define_accessor(ctx, style_proto, "transform", style_get_empty, style_set_transform);
    JS_SetClassProto(ctx, impl->style_class_id, style_proto);

    JSValue proto = JS_NewObject(ctx);
    define_method(ctx, proto, "AddClass", panel_add_class, 1);
    define_method(ctx, proto, "RemoveClass", panel_remove_class, 1);
    define_method(ctx, proto, "ToggleClass", panel_toggle_class, 1);
    define_method(ctx, proto, "TriggerClass", panel_trigger_class, 1);
    define_method(ctx, proto, "SetHasClass", panel_set_has_class, 2);
    define_method(ctx, proto, "BHasClass", panel_has_class, 1);
    define_method(ctx, proto, "SwitchClass", panel_switch_class, 2);
    define_method(ctx, proto, "FindChild", panel_find_child, 1);
    define_method(ctx, proto, "FindChildTraverse", panel_find_child, 1);
    define_method(ctx, proto, "FindChildInLayoutFile", panel_find_child, 1);
    define_method(ctx, proto, "FindChildrenWithClassTraverse", panel_find_children_with_class, 1);
    define_method(ctx, proto, "GetChild", panel_get_child, 1);
    define_method(ctx, proto, "GetChildCount", panel_get_child_count, 0);
    define_method(ctx, proto, "Children", panel_children, 0);
    define_method(ctx, proto, "GetParent", panel_get_parent, 0);
    define_method(ctx, proto, "SetParent", panel_set_parent, 1);
    define_method(ctx, proto, "MoveChildBefore", panel_move_child_before, 2);
    define_method(ctx, proto, "MoveChildAfter", panel_move_child_after, 2);
    define_method(ctx, proto, "RemoveAndDeleteChildren", panel_remove_and_delete_children, 0);
    define_method(ctx, proto, "DeleteAsync", panel_delete_async, 1);
    define_method(ctx, proto, "Data", panel_data, 0);
    define_method(ctx, proto, "SetAttributeString", panel_set_attribute, 2);
    define_method(ctx, proto, "GetAttributeString", panel_get_attribute, 2);
    define_method(ctx, proto, "SetImage", panel_set_image, 1);
    define_method(ctx, proto, "SetMovie", panel_set_movie, 1);
    define_method(ctx, proto, "SetSound", panel_set_sound, 1);
    define_method(ctx, proto, "Play", panel_play, 0);
    define_method(ctx, proto, "Pause", panel_pause, 1);
    define_method(ctx, proto, "SetDialogVariable", panel_set_dialog_variable, 2);
    define_method(ctx, proto, "SetDialogVariableInt", panel_set_dialog_variable, 2);
    define_method(ctx, proto, "SetDialogVariableTime", panel_set_dialog_variable, 2);
    define_method(ctx, proto, "BLoadLayout", panel_load_layout, 3);
    define_method(ctx, proto, "LoadLayout", panel_load_layout, 3);
    define_method(ctx, proto, "LoadLayoutAsync", panel_load_layout, 3);
    define_method(ctx, proto, "BHasLayoutSnippet", panel_has_layout_snippet, 1);
    define_method(ctx, proto, "BLoadLayoutSnippet", panel_load_snippet, 1);
    define_method(ctx, proto, "BCreateChildren", panel_load_snippet, 1);
    define_method(ctx, proto, "IsValid", panel_is_valid, 0);
    define_method(ctx, proto, "IsSelected", panel_is_selected, 0);
    define_method(ctx, proto, "SetSelected", panel_set_selected_method, 1);
    define_method(ctx, proto, "GetSelected", panel_get_selected, 0);
    define_method(ctx, proto, "AddOption", panel_add_option, 1);
    define_method(ctx, proto, "RemoveAllOptions", panel_remove_all_options, 0);
    define_method(ctx, proto, "SetSelectedIndex", panel_set_selected_index, 1);
    define_method(ctx, proto, "AccessDropDownMenu", panel_access_dropdown_menu, 0);
    define_method(ctx, proto, "BIsTransparent", panel_b_is_transparent, 0);
    define_method(ctx, proto, "SetFocus", panel_noop, 0);
    define_method(ctx, proto, "Focus", panel_noop, 0);
    define_method(ctx, proto, "ScrollToTop", panel_scroll_to_top, 0);
    define_method(ctx, proto, "ScrollToBottom", panel_scroll_to_bottom, 0);
    define_method(ctx, proto, "ScrollParentToMakePanelFit", panel_scroll_parent_to_make_panel_fit, 2);
    define_method(ctx, proto, "ScrollParentToFitWhenFocused", panel_noop, 1);
    define_method(ctx, proto, "SetAutoScrollEnabled", panel_noop, 1);
    define_method(ctx, proto, "RegisterForReadyEvents", panel_noop, 1);
    define_method(ctx, proto, "SetScene", panel_noop, 3);
    define_method(ctx, proto, "SetSceneAngles", panel_noop, 4);
    define_method(ctx, proto, "SetPlayerCharacterItemID", panel_noop, 1);
    define_method(ctx, proto, "SetPlayerModel", panel_noop, 1);
    define_method(ctx, proto, "SetCameraPreset", panel_noop, 2);
    define_method(ctx, proto, "RestoreLightingState", panel_noop, 0);
    define_method(ctx, proto, "SetFlashlightAmount", panel_noop, 1);
    define_method(ctx, proto, "SetFlashlightColor", panel_noop, 3);
    define_method(ctx, proto, "SetFlashlightFOV", panel_noop, 1);
    define_method(ctx, proto, "SetAmbientLightColor", panel_noop, 3);
    define_method(ctx, proto, "SetDirectionalLightModify", panel_noop, 1);
    define_method(ctx, proto, "SetDirectionalLightColor", panel_noop, 3);
    define_method(ctx, proto, "SetDirectionalLightDirection", panel_noop, 3);
    define_method(ctx, proto, "SetDirectionalLightPulseFlicker", panel_noop, 4);
    define_method(ctx, proto, "SetReadyForDisplay", panel_set_ready_for_display, 1);
    define_method(ctx, proto, "SetPanelEvent", panel_set_panel_event, 2);
    define_accessor(ctx, proto, "id", panel_get_id, nullptr);
    define_accessor(ctx, proto, "visible", panel_get_visible, panel_set_visible);
    define_accessor(ctx, proto, "enabled", panel_get_enabled, panel_set_enabled);
    define_accessor(ctx, proto, "checked", panel_get_checked, panel_set_checked);
    define_accessor(ctx, proto, "selected", panel_get_selected_prop, panel_set_selected_prop);
    define_accessor(ctx, proto, "group", panel_get_group, panel_set_group);
    define_accessor(ctx, proto, "tooltip", panel_get_tooltip, panel_set_tooltip);
    define_accessor(ctx, proto, "marked_for_delete", panel_get_marked_for_delete, panel_set_marked_for_delete);
    define_accessor(ctx, proto, "steamid", panel_get_steamid, panel_set_steamid);
    define_accessor(ctx, proto, "xuid", panel_get_xuid, panel_set_xuid);
    define_accessor(ctx, proto, "text", panel_get_text, panel_set_text);
    define_accessor(ctx, proto, "scrolloffset_x", panel_get_scroll_offset_x, nullptr);
    define_accessor(ctx, proto, "scrolloffset_y", panel_get_scroll_offset_y, nullptr);
    define_accessor(ctx, proto, "style", panel_get_style, nullptr);
    JS_SetClassProto(ctx, impl->panel_class_id, proto);

    // Build the callable `$` global: `$('#id')` selects a panel; `$.Msg(...)` etc.
    // hang off it as properties.
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue dollar = JS_NewCFunction(ctx, dollar_select, "$", 1);
    define_method(ctx, dollar, "Msg", dollar_msg, 1);
    define_method(ctx, dollar, "Warning", dollar_warning, 1);
    define_method(ctx, dollar, "Localize", dollar_localize, 1);
    define_method(ctx, dollar, "GetContextPanel", dollar_get_context_panel, 0);
    define_method(ctx, dollar, "CreatePanel", dollar_create_panel, 3);
    define_method(ctx, dollar, "RegisterEventHandler", dollar_register_event_handler, 3);
    define_method(ctx, dollar, "RegisterForUnhandledEvent", dollar_register_event_handler, 2);
    define_method(ctx, dollar, "DispatchEvent", dollar_dispatch_event, 1);
    define_method(ctx, dollar, "Schedule", dollar_schedule, 2);
    define_method(ctx, dollar, "CancelScheduled", dollar_cancel_scheduled, 1);
    define_method(ctx, dollar, "__host", dollar_host, 2);
    JS_SetPropertyStr(ctx, global, "$", dollar);
    JS_FreeValue(ctx, global);

    impl_ = std::move(impl);

    // Core prelude first, then host bootstrap scripts, then the document's
    // scripts in declared order. The host layer is where content-specific API
    // shims live, like CEF's client-provided handlers around a reusable browser.
    run_script_source(kPanoramaCorePrelude, "panorama://core-prelude");

    if (impl_->client != nullptr)
    {
        for (const PanoramaRuntimeScript& script : impl_->client->bootstrap_scripts())
        {
            run_script_source(script.source, script.origin.empty() ? "panorama://host-bootstrap" : script.origin.c_str());
        }
    }
    for (const PanoramaRuntimeScript& script : impl_->bootstrap_scripts)
    {
        run_script_source(script.source, script.origin.empty() ? "panorama://host-bootstrap" : script.origin.c_str());
    }

    const auto read_script = [&](const std::string& path) -> std::string {
        if (std::optional<std::string> script = resources.read_text(path))
        {
            return *script;
        }
        return {};
    };

    int loaded = 0;
    for (const PanoramaRuntimeScriptInclude& script : scripts)
    {
        const std::string source = read_script(script.path);
        if (source.empty())
        {
            pano_log_warning("[panorama] script not found: {}", script.path);
            continue;
        }
        // Run with the script's own layout root as $.GetContextPanel() (frame
        // sublayouts get the frame panel); null context = the document root.
        impl_->run_code(script.context, source, ("panorama://" + script.path).c_str());
        ++loaded;
    }

    // Scripts almost always mutate the DOM during init; force a recompute.
    impl_->dirty = true;

    pano_log_info("[panorama] runtime started: {} script(s), {} event handler(s)", loaded, impl_->event_handlers.size());
    return true;
}

void PanoramaRuntime::set_host_action_handler(HostActionHandler handler)
{
    host_action_ = std::move(handler);
    if (impl_)
    {
        impl_->host_action = host_action_;
    }
}

void PanoramaRuntime::set_client(PanoramaRuntimeClient* client)
{
    client_ = client;
    if (impl_)
    {
        impl_->client = client_;
    }
}

void PanoramaRuntime::set_bootstrap_scripts(std::vector<PanoramaRuntimeScript> scripts)
{
    bootstrap_scripts_ = std::move(scripts);
    if (impl_)
    {
        impl_->bootstrap_scripts = bootstrap_scripts_;
    }
}

void PanoramaRuntime::set_layout_loaders(LayoutLoader file_loader, LayoutLoader snippet_loader, SnippetExists snippet_exists)
{
    file_loader_ = std::move(file_loader);
    snippet_loader_ = std::move(snippet_loader);
    snippet_exists_ = std::move(snippet_exists);
    if (impl_)
    {
        impl_->load_layout_file = file_loader_;
        impl_->load_layout_snippet = snippet_loader_;
        impl_->has_layout_snippet = snippet_exists_;
    }
}

void PanoramaRuntime::run_source_in_context(const std::string& source, const std::string& origin, PanoramaNode& context)
{
    if (!active())
    {
        return;
    }
    impl_->run_code(&context, source, origin.c_str());
    JSContext* job_ctx = nullptr;
    while (JS_ExecutePendingJob(impl_->rt, &job_ctx) > 0)
    {
    }
}

void PanoramaRuntime::run_node_handler(PanoramaNode& node, const std::string& event_attr)
{
    if (!active())
    {
        return;
    }
    const auto it = node.attributes.find(event_attr);
    if (it != node.attributes.end() && !it->second.empty() && it->second != kPanelEventMarker)
    {
        // Run with the layout root as the context panel: Panorama's GetContextPanel()
        // (and thus $.FindChildInContext) resolves against the script's owning layout,
        // not the event target, so handlers can find siblings elsewhere in the tree.
        impl_->run_code(impl_->document, it->second, event_attr.c_str());
    }

    const auto node_events = impl_->panel_events.find(&node);
    if (node_events != impl_->panel_events.end())
    {
        const auto event = node_events->second.find(event_attr);
        if (event != node_events->second.end())
        {
            // Hold our own reference: the handler may delete its own panel,
            // which frees the registry's reference mid-call.
            JSValue fn = JS_DupValue(impl_->ctx, event->second);
            JSValue result = JS_Call(impl_->ctx, fn, JS_UNDEFINED, 0, nullptr);
            if (JS_IsException(result))
            {
                impl_->dump_pending_error(event_attr.c_str());
            }
            JS_FreeValue(impl_->ctx, result);
            JS_FreeValue(impl_->ctx, fn);
        }
    }
    JSContext* job_ctx = nullptr;
    while (JS_ExecutePendingJob(impl_->rt, &job_ctx) > 0)
    {
    }
}

void PanoramaRuntime::run_script_source(const std::string& source, const char* origin)
{
    JSContext* ctx = impl_->ctx;
    JSValue result = JS_Eval(ctx, source.c_str(), source.size(), origin, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result))
    {
        impl_->dump_pending_error(origin);
    }
    JS_FreeValue(ctx, result);

    // Drain any micro-tasks (promise jobs) the script queued.
    JSContext* job_ctx = nullptr;
    while (JS_ExecutePendingJob(impl_->rt, &job_ctx) > 0)
    {
    }
}

void PanoramaRuntime::update(double dt_seconds)
{
    if (!active())
    {
        return;
    }

    JSContext* ctx = impl_->ctx;

    // Collect callbacks that are due, then fire outside the scan so a callback may
    // reschedule without disturbing iteration.
    std::vector<Impl::Scheduled> due;
    for (auto it = impl_->scheduled.begin(); it != impl_->scheduled.end();)
    {
        it->remaining -= dt_seconds;
        if (it->remaining <= 0.0)
        {
            due.push_back(*it);
            it = impl_->scheduled.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // Register the due batch so a callback that deletes another callback's
    // context panel nulls the stale pointer here too, not only in `scheduled`.
    impl_->firing_scheduled = &due;
    for (const Impl::Scheduled& entry : due)
    {
        // The callback sees its registering script's context panel.
        Impl::ContextScope scope(*impl_, entry.context);
        JSValue result = JS_Call(ctx, entry.fn, JS_UNDEFINED, 0, nullptr);
        if (JS_IsException(result))
        {
            impl_->dump_pending_error("scheduled callback");
        }
        JS_FreeValue(ctx, result);
        JS_FreeValue(ctx, entry.fn);
    }
    impl_->firing_scheduled = nullptr;

    JSContext* job_ctx = nullptr;
    while (JS_ExecutePendingJob(impl_->rt, &job_ctx) > 0)
    {
    }
}

void PanoramaRuntime::dispatch_event(const std::string& event_name)
{
    if (active())
    {
        impl_->fire(event_name, 0, nullptr);
    }
}

void PanoramaRuntime::dispatch_property_transition_end(PanoramaNode& panel, const char* property)
{
    if (!active() || property == nullptr)
    {
        return;
    }
    // Panorama signature: handler(panelName, propertyName) — panelName is the
    // panel's id string (mainmenu.js compares `newPanel.id === panelName`).
    // Delivery is scoped to handlers registered for `panel` (plus unscoped ones).
    JSContext* ctx = impl_->ctx;
    JSValue argv[2] = {
        JS_NewStringLen(ctx, panel.id.data(), panel.id.size()),
        JS_NewString(ctx, property),
    };
    impl_->fire("PropertyTransitionEnd", 2, argv, &panel);
    JS_FreeValue(ctx, argv[0]);
    JS_FreeValue(ctx, argv[1]);
}

void PanoramaRuntime::shutdown()
{
    if (!impl_)
    {
        return;
    }

    if (impl_->ctx != nullptr)
    {
        JSContext* ctx = impl_->ctx;
        for (auto& [name, handlers] : impl_->event_handlers)
        {
            for (const Impl::EventHandler& handler : handlers)
            {
                JS_FreeValue(ctx, handler.fn);
            }
        }
        for (auto& entry : impl_->scheduled)
        {
            JS_FreeValue(ctx, entry.fn);
        }
        for (auto& [node, data] : impl_->panel_data)
        {
            JS_FreeValue(ctx, data);
        }
        for (auto& [node, events] : impl_->panel_events)
        {
            for (auto& [name, fn] : events)
            {
                JS_FreeValue(ctx, fn);
            }
        }
        for (auto& [node, wrapper] : impl_->panel_wrappers)
        {
            // Neutralize before release: script-side references that outlive the
            // runtime must not resurface a node pointer.
            JS_SetOpaque(wrapper, nullptr);
            JS_FreeValue(ctx, wrapper);
        }
    }
    impl_->event_handlers.clear();
    impl_->scheduled.clear();
    impl_->panel_data.clear();
    impl_->panel_events.clear();
    impl_->panel_wrappers.clear();
    impl_->switch_classes.clear();

    if (impl_->ctx != nullptr)
    {
        JS_FreeContext(impl_->ctx);
        impl_->ctx = nullptr;
    }
    if (impl_->rt != nullptr)
    {
        JS_FreeRuntime(impl_->rt);
        impl_->rt = nullptr;
    }

    impl_.reset();
}
}
