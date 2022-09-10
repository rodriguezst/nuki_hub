#include "NukiOpenerWrapper.h"
#include <RTOS.h>
#include "PreferencesKeys.h"
#include "MqttTopics.h"
#include <NukiOpenerUtils.h>

NukiOpenerWrapper* nukiOpenerInst;

NukiOpenerWrapper::NukiOpenerWrapper(const std::string& deviceName, uint32_t id, BleScanner::Scanner* scanner,  NetworkOpener* network, Preferences* preferences)
: _deviceName(deviceName),
  _nukiOpener(deviceName, id),
  _bleScanner(scanner),
  _network(network),
  _preferences(preferences)
{
    nukiOpenerInst = this;

    memset(&_lastKeyTurnerState, sizeof(NukiLock::KeyTurnerState), 0);
    memset(&_lastBatteryReport, sizeof(NukiLock::BatteryReport), 0);
    memset(&_batteryReport, sizeof(NukiLock::BatteryReport), 0);
    memset(&_keyTurnerState, sizeof(NukiLock::KeyTurnerState), 0);
    _keyTurnerState.lockState = NukiOpener::LockState::Undefined;

    network->setLockActionReceivedCallback(nukiOpenerInst->onLockActionReceivedCallback);
    network->setConfigUpdateReceivedCallback(nukiOpenerInst->onConfigUpdateReceivedCallback);
}


NukiOpenerWrapper::~NukiOpenerWrapper()
{
    _bleScanner = nullptr;
}


void NukiOpenerWrapper::initialize()
{
    _nukiOpener.initialize();
    _nukiOpener.registerBleScanner(_bleScanner);

    _intervalLockstate = _preferences->getInt(preference_query_interval_lockstate);
    _intervalBattery = _preferences->getInt(preference_query_interval_battery);
    _publishAuthData = _preferences->getBool(preference_publish_authdata);

    if(_intervalLockstate == 0)
    {
        _intervalLockstate = 60 * 5;
        _preferences->putInt(preference_query_interval_lockstate, _intervalLockstate);
    }
    if(_intervalBattery == 0)
    {
        _intervalBattery = 60 * 30;
        _preferences->putInt(preference_query_interval_battery, _intervalBattery);
    }

    _nukiOpener.setEventHandler(this);

    Serial.print(F("Lock state interval: "));
    Serial.print(_intervalLockstate);
    Serial.print(F(" | Battery interval: "));
    Serial.print(_intervalBattery);
    Serial.print(F(" | Publish auth data: "));
    Serial.println(_publishAuthData ? "yes" : "no");

    if(!_publishAuthData)
    {
        _clearAuthData = true;
    }
}

void NukiOpenerWrapper::update()
{
    if (!_paired) {
        Serial.println(F("Nuki opener start pairing"));

        if (_nukiOpener.pairNuki() == NukiOpener::PairingResult::Success) {
            Serial.println(F("Nuki opener paired"));
            _paired = true;
            setupHASS();
        }
        else
        {
            vTaskDelay( 200 / portTICK_PERIOD_MS);
            return;
        }
    }

    _nukiOpener.updateConnectionState();

    unsigned long ts = millis();

    if(_statusUpdated || _nextLockStateUpdateTs == 0 || ts >= _nextLockStateUpdateTs)
    {
        _nextLockStateUpdateTs = ts + _intervalLockstate * 1000;
        updateKeyTurnerState();
        _statusUpdated = false;
    }
    if(_nextBatteryReportTs == 0 || ts > _nextBatteryReportTs)
    {
        _nextBatteryReportTs = ts + _intervalBattery * 1000;
        updateBatteryState();
    }
    if(_nextConfigUpdateTs == 0 || ts > _nextConfigUpdateTs)
    {
        _nextConfigUpdateTs = ts + _intervalConfig * 1000;
        updateConfig();
    }
    if(_nextRssiTs == 0 || ts > _nextRssiTs)
    {
        _nextRssiTs = ts + 3000;

        int rssi = _nukiOpener.getRssi();
        if(rssi != _lastRssi)
        {
            _network->publishRssi(rssi);
            _lastRssi = rssi;
        }
    }

    if(_nextLockAction != (NukiOpener::LockAction)0xff)
    {
        NukiOpener::CmdResult cmdResult = _nukiOpener.lockAction(_nextLockAction, 0, 0);

        char resultStr[15] = {0};
        NukiOpener::cmdResultToString(cmdResult, resultStr);

        _network->publishCommandResult(resultStr);

        Serial.print(F("Opener lock action result: "));
        Serial.println(resultStr);

        _nextLockAction = (NukiOpener::LockAction)0xff;
        if(_intervalLockstate > 10)
        {
            _nextLockStateUpdateTs = ts + 10 * 1000;
        }
    }

    if(_clearAuthData)
    {
        _network->clearAuthorizationInfo();
        _clearAuthData = false;
    }

    memcpy(&_lastKeyTurnerState, &_keyTurnerState, sizeof(NukiOpener::OpenerState));
}

void NukiOpenerWrapper::setPin(const uint16_t pin)
{
    _nukiOpener.saveSecurityPincode(pin);
}

void NukiOpenerWrapper::unpair()
{
    _nukiOpener.unPairNuki();
    _paired = false;
}

void NukiOpenerWrapper::updateKeyTurnerState()
{
    _nukiOpener.requestOpenerState(&_keyTurnerState);

    if(_statusUpdated && _keyTurnerState.lockState == NukiOpener::LockState::Locked && _lastKeyTurnerState.lockState == NukiOpener::LockState::Locked)
    {
        Serial.println(F("Nuki opener: Ring detected"));
        _network->publishRing();
    }
    else
    {
        _network->publishKeyTurnerState(_keyTurnerState, _lastKeyTurnerState);

        if(_keyTurnerState.lockState != _lastKeyTurnerState.lockState)
        {
            char lockStateStr[20];
            lockstateToString(_keyTurnerState.lockState, lockStateStr);
            Serial.print(F("Nuki opener state: "));
            Serial.println((int)_keyTurnerState.lockState);
        }
    }

    if(_publishAuthData)
    {
        updateAuthData();
    }
}

void NukiOpenerWrapper::updateBatteryState()
{
    _nukiOpener.requestBatteryReport(&_batteryReport);
    _network->publishBatteryReport(_batteryReport);
}

void NukiOpenerWrapper::updateConfig()
{
    readConfig();
    readAdvancedConfig();
    _network->publishConfig(_nukiConfig);
    _network->publishAdvancedConfig(_nukiAdvancedConfig);
}

void NukiOpenerWrapper::updateAuthData()
{
    Nuki::CmdResult result = _nukiOpener.retrieveLogEntries(0, 0, 0, true);
    if(result != Nuki::CmdResult::Success)
    {
        return;
    }
    delay(100);

    uint16_t count = _nukiOpener.getLogEntryCount();

    result = _nukiOpener.retrieveLogEntries(0, count < 5 ? count : 5, 1, false);
    if(result != Nuki::CmdResult::Success)
    {
        return;
    }
    delay(1000);

    std::list<NukiOpener::LogEntry> log;
    _nukiOpener.getLogEntries(&log);

    if(log.size() > 0)
    {
        _network->publishAuthorizationInfo(log);
    }
}

NukiOpener::LockAction NukiOpenerWrapper::lockActionToEnum(const char *str)
{
    if(strcmp(str, "activateRTO") == 0) return NukiOpener::LockAction::ActivateRTO;
    else if(strcmp(str, "deactivateRTO") == 0) return NukiOpener::LockAction::DeactivateRTO;
    else if(strcmp(str, "electricStrikeActuation") == 0) return NukiOpener::LockAction::ElectricStrikeActuation;
    else if(strcmp(str, "activateCM") == 0) return NukiOpener::LockAction::ActivateCM;
    else if(strcmp(str, "deactivateCM") == 0) return NukiOpener::LockAction::DeactivateCM;
    else if(strcmp(str, "fobAction2") == 0) return NukiOpener::LockAction::FobAction2;
    else if(strcmp(str, "fobAction1") == 0) return NukiOpener::LockAction::FobAction1;
    else if(strcmp(str, "fobAction3") == 0) return NukiOpener::LockAction::FobAction3;
    return (NukiOpener::LockAction)0xff;
}

bool NukiOpenerWrapper::onLockActionReceivedCallback(const char *value)
{
    NukiOpener::LockAction action = nukiOpenerInst->lockActionToEnum(value);
    nukiOpenerInst->_nextLockAction = action;
    return (int)action != 0xff;
}

void NukiOpenerWrapper::onConfigUpdateReceivedCallback(const char *topic, const char *value)
{
    nukiOpenerInst->onConfigUpdateReceived(topic, value);
}


void NukiOpenerWrapper::onConfigUpdateReceived(const char *topic, const char *value)
{
    if(strcmp(topic, mqtt_topic_config_button_enabled) == 0)
    {
        bool newValue = atoi(value) > 0;
        if(!_nukiConfigValid || _nukiConfig.buttonEnabled == newValue) return;
        _nukiOpener.enableButton(newValue);
        _nextConfigUpdateTs = millis() + 300;
    }
    if(strcmp(topic, mqtt_topic_config_led_enabled) == 0)
    {
        bool newValue = atoi(value) > 0;
        if(!_nukiConfigValid || _nukiConfig.ledFlashEnabled == newValue) return;
        _nukiOpener.enableLedFlash(newValue);
        _nextConfigUpdateTs = millis() + 300;
    }
    if(strcmp(topic, mqtt_topic_config_sound_level) == 0)
    {
        uint8_t newValue = atoi(value);
        if(!_nukiAdvancedConfigValid || _nukiAdvancedConfig.soundLevel == newValue) return;
        _nukiOpener.setSoundLevel(newValue);
        _nextConfigUpdateTs = millis() + 300;
    }
}

const NukiOpener::OpenerState &NukiOpenerWrapper::keyTurnerState()
{
    return _keyTurnerState;
}

const bool NukiOpenerWrapper::isPaired()
{
    return _paired;
}

BleScanner::Scanner *NukiOpenerWrapper::bleScanner()
{
    return _bleScanner;
}

void NukiOpenerWrapper::notify(Nuki::EventType eventType)
{
    if(eventType == Nuki::EventType::KeyTurnerStatusUpdated)
    {
        _statusUpdated = true;
    }
}

void NukiOpenerWrapper::readConfig()
{
    Serial.print(F("Reading opener config. Result: "));
    Nuki::CmdResult result = _nukiOpener.requestConfig(&_nukiConfig);
    _nukiConfigValid = result == Nuki::CmdResult::Success;
    Serial.println(result);
}

void NukiOpenerWrapper::readAdvancedConfig()
{
    Serial.print(F("Reading opener advanced config. Result: "));
    Nuki::CmdResult result = _nukiOpener.requestAdvancedConfig(&_nukiAdvancedConfig);
    _nukiAdvancedConfigValid = result == Nuki::CmdResult::Success;
    Serial.println(result);
}

void NukiOpenerWrapper::setupHASS()
{
    if(!_nukiConfigValid) // only ask for config once to save battery life
    {
        Nuki::CmdResult result = _nukiOpener.requestConfig(&_nukiConfig);
        _nukiConfigValid = result == Nuki::CmdResult::Success;
    }
    if (_nukiConfigValid)
    {
        String baseTopic = _preferences->getString(preference_mqtt_opener_path);
        char uidString[20];
        itoa(_nukiConfig.nukiId, uidString, 16);
        _network->publishHASSConfig("Opener",baseTopic.c_str(),(char*)_nukiConfig.name,uidString,"deactivateRTO","activateRTO","electricStrikeActuation","locked","unlocked");
    }
    else
    {
        Serial.println(F("Unable to setup HASS. Invalid config received."));
    }
}

void NukiOpenerWrapper::disableHASS()
{
    if(!_nukiConfigValid) // only ask for config once to save battery life
    {
        Nuki::CmdResult result = _nukiOpener.requestConfig(&_nukiConfig);
        _nukiConfigValid = result == Nuki::CmdResult::Success;
    }
    if (_nukiConfigValid)
    {
        String baseTopic = _preferences->getString(preference_mqtt_opener_path);
        char uidString[20];
        itoa(_nukiConfig.nukiId, uidString, 16);
        _network->removeHASSConfig(uidString);
    }
    else
    {
        Serial.println(F("Unable to disable HASS. Invalid config received."));
    }
}
