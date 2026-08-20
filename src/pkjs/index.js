/**
 * PebbleKit JS companion for the HTC Flip Clock watchface.
 *
 * - Uses Clay (@rebble/clay) for the settings page (units, weather refresh
 *   interval, clock style, and background wallpaper). The wallpaper picker
 *   is a stock Clay "select" dropdown (messageKey "Wallpaper", values 0-5)
 *   with a "text" item right below it showing all 6 thumbnails inline as
 *   reference, laid out 3-per-row - same pattern used in the Hackers95
 *   watchface, since stock Clay has no built-in image-radio component. The
 *   Background section is deliberately last on the settings page (below
 *   Units/Weather/Clock) since its thumbnail grid is the tallest part of
 *   the page.
 * - Location is fully automatic: the phone's own GPS (via
 *   navigator.geolocation) supplies latitude/longitude, which are used
 *   directly for the Open-Meteo Forecast API call (no separate geocoding
 *   step needed) and reverse-geocoded through BigDataCloud's free
 *   client-side API to get a human-readable place name (its "locality"
 *   field) for display. There's no location field in settings anymore -
 *   both weather and the displayed location name always track wherever the
 *   phone actually is.
 * - Sends TEMPERATURE (int), WEATHER_ICON (int 0-8), LOCATION_NAME
 *   (string), TEMP_HIGH/TEMP_LOW (int, today's forecast high/low - feeds
 *   the stats panel's weather row) to the watch via AppMessage.
 * - Also sends PHONE_BATTERY (int 0-100) and PHONE_CHARGING (0/1) for the
 *   stats panel's phone battery row, via the phone's own Battery Status API
 *   (navigator.getBattery()/navigator.battery) - see initPhoneBattery()/
 *   sendPhoneBattery() below (the event-driven structure is ported from
 *   the MetroWP8 watchface; the AppMessage send itself uses plain string
 *   keys through sendToWatch() like everything else here, since testing
 *   on real hardware showed MetroWP8's messageKeys.* global isn't defined
 *   in this build). Event-driven, not polled: initPhoneBattery() attaches
 *   levelchange/chargingchange listeners once on 'ready', so the watch
 *   gets a fresh reading pushed automatically whenever the phone's
 *   battery actually changes. Steps/sleep/watch-battery for the rest of
 *   that panel are all read natively on the watch instead (HealthService/
 *   BatteryStateService), no JS involvement needed for those three.
 */

var Clay = require('@rebble/clay');
var clayConfig = require('./config');
var clay = new Clay(clayConfig);

var DEFAULT_FAHRENHEIT = false;
var DEFAULT_WALLPAPER = 0;
var DEFAULT_BOLD_CLOCK_FONT = false;
var DEFAULT_RAINDROPS_ENABLED = true;
var DEFAULT_WEATHER_UPDATE_INTERVAL = 30;
var FALLBACK_LOCATION_NAME = '----';

// ----------------------------------------------------------------------
// Small helpers
// ----------------------------------------------------------------------

var xhrRequest = function (url, type, callback, errorCallback) {
  var xhr = new XMLHttpRequest();
  xhr.timeout = 15000;
  xhr.onload = function () {
    if (xhr.status >= 200 && xhr.status < 300) {
      callback(xhr.responseText);
    } else if (errorCallback) {
      errorCallback('HTTP ' + xhr.status);
    }
  };
  xhr.onerror = function () {
    if (errorCallback) errorCallback('Network error');
  };
  xhr.ontimeout = function () {
    if (errorCallback) errorCallback('Timeout');
  };
  xhr.open(type, url);
  xhr.send();
};

function loadSettings() {
  var storedUnit = localStorage.getItem('UseFahrenheit');
  var useFahrenheit = storedUnit === null ? DEFAULT_FAHRENHEIT : (storedUnit === 'true');
  var storedWallpaper = localStorage.getItem('Wallpaper');
  var wallpaper = storedWallpaper === null ? DEFAULT_WALLPAPER : parseInt(storedWallpaper, 10);
  var storedBoldClockFont = localStorage.getItem('BoldClockFont');
  var boldClockFont = storedBoldClockFont === null ? DEFAULT_BOLD_CLOCK_FONT : (storedBoldClockFont === 'true');
  var storedRaindropsEnabled = localStorage.getItem('RaindropsEnabled');
  var raindropsEnabled = storedRaindropsEnabled === null ? DEFAULT_RAINDROPS_ENABLED : (storedRaindropsEnabled === 'true');
  var storedWeatherUpdateInterval = localStorage.getItem('WeatherUpdateInterval');
  var weatherUpdateInterval = storedWeatherUpdateInterval === null ? DEFAULT_WEATHER_UPDATE_INTERVAL : parseInt(storedWeatherUpdateInterval, 10);
  return {
    useFahrenheit: useFahrenheit,
    wallpaper: wallpaper,
    boldClockFont: boldClockFont,
    raindropsEnabled: raindropsEnabled,
    weatherUpdateInterval: weatherUpdateInterval
  };
}

function saveSettings(dict) {
  if (typeof dict.UseFahrenheit !== 'undefined') {
    localStorage.setItem('UseFahrenheit', dict.UseFahrenheit ? 'true' : 'false');
  }
  if (typeof dict.Wallpaper !== 'undefined') {
    localStorage.setItem('Wallpaper', dict.Wallpaper);
  }
  if (typeof dict.BoldClockFont !== 'undefined') {
    localStorage.setItem('BoldClockFont', dict.BoldClockFont ? 'true' : 'false');
  }
  if (typeof dict.RaindropsEnabled !== 'undefined') {
    localStorage.setItem('RaindropsEnabled', dict.RaindropsEnabled ? 'true' : 'false');
  }
  if (typeof dict.WeatherUpdateInterval !== 'undefined') {
    localStorage.setItem('WeatherUpdateInterval', dict.WeatherUpdateInterval);
  }
}

// Maps a WMO weather_code + is_day flag to one of the 9 bundled icons.
// Index order must match WEATHER_RESOURCE_IDS[] in src/c/main.c:
// 0 clear-day, 1 clear-night, 2 partly-day, 3 partly-night, 4 cloudy,
// 5 fog, 6 rain, 7 snow, 8 thunder.
function weatherCodeToIconIndex(code, isDay) {
  var day = isDay === 1 || isDay === true;
  if (code === 0) return day ? 0 : 1;
  if (code === 1 || code === 2) return day ? 2 : 3;
  if (code === 3) return 4;
  if (code === 45 || code === 48) return 5;
  if (code <= 67) return 6;       // drizzle, rain, freezing rain
  if (code <= 77) return 7;       // snow, snow grains
  if (code === 80 || code === 81 || code === 82) return 6; // rain showers
  if (code === 85 || code === 86) return 7; // snow showers
  if (code >= 95) return 8;       // thunderstorm
  return 4;
}

// ----------------------------------------------------------------------
// Weather fetch: GPS position -> BigDataCloud reverse geocode (name) +
// Open-Meteo forecast (conditions), both keyed off the same coordinates.
// ----------------------------------------------------------------------

function sendToWatch(dict) {
  Pebble.sendAppMessage(dict,
    function () { console.log('Sent to watch: ' + JSON.stringify(dict)); },
    function (e) { console.log('Failed to send to watch: ' + JSON.stringify(e)); }
  );
}

// BigDataCloud's "reverse-geocode-client" endpoint is meant for exactly
// this - free, no API key, client-side reverse geocoding. We evaluate
// 'locality' first (e.g. "Amsterdam"), falling back to city/principal
// subdivision/country name for the rare spot where locality is empty
// (open ocean, sparse areas, etc).
function reverseGeocode(lat, lon, callback) {
  var url = 'https://api.bigdatacloud.net/data/reverse-geocode-client?latitude=' +
      lat + '&longitude=' + lon + '&localityLanguage=en';

  xhrRequest(url, 'GET', function (text) {
    var data;
    try {
      data = JSON.parse(text);
    } catch (e) {
      console.log('Reverse geocode parse error: ' + e);
      callback(FALLBACK_LOCATION_NAME);
      return;
    }

    var name = data.locality || data.city || data.principalSubdivision ||
        data.countryName || FALLBACK_LOCATION_NAME;
    if (data.locality && data.countryCode) {
      name = data.locality + ', ' + data.countryCode;
    }
    callback(name);
  }, function (err) {
    console.log('Reverse geocode request failed: ' + err);
    callback(FALLBACK_LOCATION_NAME);
  });
}

function fetchForecastAndSend(lat, lon, locationName) {
  // Always requested (and sent to the watch) in Celsius, regardless of the
  // "Use Fahrenheit" Clay setting - the watch itself now converts for
  // display (celsius_to_fahrenheit()/display_temp() in src/c/main.c).
  // Previously this passed temperature_unit=fahrenheit when the toggle was
  // on, which meant flipping the toggle only actually changed what was
  // shown once a brand new GPS+network weather fetch completed (and never
  // did if that fetch failed or the phone had no signal) - the watch just
  // relabeled whatever number it already had with the new unit letter in
  // the meantime, which read as "the toggle doesn't recalculate anything".
  // Fetching in one fixed unit and converting on-watch makes the toggle
  // take effect immediately, with no network round-trip involved.
  var wUrl = 'https://api.open-meteo.com/v1/forecast?latitude=' + lat +
      '&longitude=' + lon +
      '&current=temperature_2m,weather_code,is_day' +
      '&daily=temperature_2m_max,temperature_2m_min' +
      '&temperature_unit=celsius' +
      '&timezone=auto';

  xhrRequest(wUrl, 'GET', function (wText) {
    var w;
    try {
      w = JSON.parse(wText);
    } catch (e) {
      console.log('Forecast response parse error: ' + e);
      return;
    }

    var temperature = Math.round(w.current.temperature_2m);
    var iconIndex = weatherCodeToIconIndex(w.current.weather_code, w.current.is_day);

    // today's entry is index 0 of the daily arrays; fall back to the
    // current temperature if daily data is ever missing so the stats
    // panel doesn't show garbage.
    var tempHigh = temperature;
    var tempLow = temperature;
    if (w.daily && w.daily.temperature_2m_max && w.daily.temperature_2m_max.length > 0) {
      tempHigh = Math.round(w.daily.temperature_2m_max[0]);
    }
    if (w.daily && w.daily.temperature_2m_min && w.daily.temperature_2m_min.length > 0) {
      tempLow = Math.round(w.daily.temperature_2m_min[0]);
    }

    sendToWatch({
      'TEMPERATURE': temperature,
      'WEATHER_ICON': iconIndex,
      'LOCATION_NAME': locationName,
      'TEMP_HIGH': tempHigh,
      'TEMP_LOW': tempLow
    });
  }, function (err) {
    console.log('Forecast request failed: ' + err);
  });
}

// ─── Phone battery ────────────────────────────────────────────────────────────
// Not a Pebble API — this is the standard HTML5 Battery Status API
// (navigator.getBattery() / the older navigator.battery). It's deprecated
// on the open web (most browsers dropped it around 2016 over fingerprinting
// concerns), but PebbleKit JS's own runtime isn't a regular browser tab and
// has historically supported it — same approach other Pebble companion
// apps have used to report phone battery to the watch. Ported from
// MetroWP8's index.js (initPhoneBattery() attaches levelchange/
// chargingchange listeners once on 'ready', so the watch gets a fresh
// reading pushed automatically whenever the phone's battery actually
// changes - no polling needed).
//
// CONFIRMED ON YOUR HARDWARE: `messageKeys` (MetroWP8's original approach
// for building the AppMessage keys) is NOT defined in this build's
// PebbleKit JS runtime, even with enableMultiJS:true - your test log
// showed "messageKeys.PHONE_BATTERY unavailable, falling back to string
// keys" followed by a successful send. So this version just sends plain
// string keys via the existing sendToWatch() helper from the start -
// same proven pattern every other message in this file already uses
// (TEMPERATURE, WEATHER_ICON, etc.) - rather than trying messageKeys.*
// first and falling back every single time.
function sendPhoneBattery(battery) {
  var pct = Math.round(battery.level * 100);
  sendToWatch({
    'PHONE_BATTERY': pct,
    'PHONE_CHARGING': battery.charging ? 1 : 0
  });
}

function initPhoneBattery() {
  function attach(battery) {
    sendPhoneBattery(battery); // send once immediately
    battery.addEventListener('levelchange', function() { sendPhoneBattery(battery); });
    battery.addEventListener('chargingchange', function() { sendPhoneBattery(battery); });
  }

  if (navigator.getBattery) {
    navigator.getBattery().then(attach, function (err) {
      console.log('Phone battery: getBattery() promise rejected: ' + err);
    });
  } else if (navigator.battery) {
    attach(navigator.battery);
  } else {
    console.log('Phone battery: Battery Status API not available in this runtime');
  }
}

function fetchWeather() {
  if (!navigator.geolocation) {
    console.log('navigator.geolocation unavailable on this phone');
    return;
  }

  navigator.geolocation.getCurrentPosition(function (pos) {
    var lat = pos.coords.latitude;
    var lon = pos.coords.longitude;
    console.log('Got position: ' + lat + ',' + lon);

    reverseGeocode(lat, lon, function (locationName) {
      fetchForecastAndSend(lat, lon, locationName);
    });
  }, function (err) {
    console.log('Geolocation failed: ' + err.message);
  }, {
    enableHighAccuracy: false,
    timeout: 15000,
    maximumAge: 10 * 60 * 1000  // reuse a fix up to 10 min old - saves GPS power
  });
}

// ----------------------------------------------------------------------
// Event wiring
// ----------------------------------------------------------------------

Pebble.addEventListener('ready', function () {
  console.log('PebbleKit JS ready');
  fetchWeather();
  initPhoneBattery();
});

Pebble.addEventListener('appmessage', function (e) {
  if (e.payload && e.payload['REQUEST_WEATHER']) {
    fetchWeather();
    // Phone battery no longer needs to be re-requested here - initPhoneBattery()
    // (called once above) keeps it live via levelchange/chargingchange listeners.
  }
});

// Clay settings page closed - persist locally and push to the watch.
Pebble.addEventListener('webviewclosed', function (e) {
  if (!e || !e.response) return;

  var dict = clay.getSettings(e.response);
  if (!dict || Object.keys(dict).length === 0) return;

  saveSettings(dict);
  sendToWatch(dict);

  // No weather refetch needed here anymore: fetchForecastAndSend() always
  // requests Celsius and the watch converts for display on its own (see
  // the comment in fetchForecastAndSend()), so a Use-Fahrenheit toggle no
  // longer needs a fresh GPS+network round-trip to take effect - it's
  // instant, driven entirely by the UseFahrenheit tuple already sent above
  // via sendToWatch(dict). None of the other Clay settings (wallpaper,
  // bold font, raindrops) touch weather data either, so there's nothing
  // left here that a refetch would actually update - the existing periodic
  // refresh (REQUEST_WEATHER from the watch, see the 'appmessage' listener
  // above) still keeps conditions themselves current.
});
