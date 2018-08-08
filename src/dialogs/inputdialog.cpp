/***
 * Copyright (c) 2018, Robert Alm Nilsson
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the organization nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ***/

#include "inputdialog.h"
#include "ui_inputdialog.h"
#include "../common.h"
#include "../error.h"
#include "../core.h"
#include "../plugin.h"

#include <SDL.h>
#include <QThread>
#include <QKeyEvent>


static QString toSectionName(const QString &name, int controllerNumber)
{
    return QString(name).remove("mupen64plus-")
        + "-control" + QString::number(controllerNumber);
}


static bool openSection(const QString &sectionName, m64p_handle &configHandle)
{
    m64p_error rval;
    rval = ConfigOpenSection(sectionName.toUtf8().data(), &configHandle);
    if (rval != M64ERR_SUCCESS) {
        SHOW_E(TR("Could not open section <Section>")
                .replace("<Section>", sectionName));
        return false;
    }
    return true;
}


InputDialog::InputDialog(const QString &name, QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::InputDialog)
    , pluginName(name)
{
    ui->setupUi(this);

    loadUnloadPlugin(name.toUtf8().data());

    for (int i = 1; i <= 4; i++) {
        m64p_handle configHandle;
        QString sectionName = toSectionName(name, i);
        openSection(sectionName, configHandle);
        controllers.push_back({sectionName, configHandle, {}, false});
        ui->controllerBox->addItem(TR("Controller <N>").replace("<N>", QString::number(i)));
    }
    connect(ui->controllerBox, SIGNAL(currentIndexChanged(int)),
            this, SLOT(controllerSelected(int)));

    connect(ui->buttonBox, SIGNAL(accepted()), this, SLOT(accept()));
    connect(ui->buttonBox, SIGNAL(rejected()), this, SLOT(reject()));

    getButtons();
    setValues();
    connectButtons();

    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK) == 0) {
        sdlWasInited = true;
    } else {
        SHOW_E(TR("Could not init SDL. Input configuration will not work. ")
                + SDL_GetError());
        sdlWasInited = false;
    }

    inputReadingState.reading = false;
}


void InputDialog::connectButtons()
{
    for (Button &b : buttons) {
        connect(b.button, &QPushButton::pressed, [&]() {
            Controller &c = currentController();
            for (Value &v : c.values) {
                if (strcmp(v.configName, b.configName) == 0) {
                    startReadInput(b, v);
                    break;
                }
            }
        });
    }
}


void InputDialog::startReadInput(Button &b, Value &v)
{
    if (inputReadingState.reading) {
        stopReadInput();
    }

    inputTimer = startTimer(50);
    SDL_Joystick *joy = SDL_JoystickOpen(0); // TODO: 0
    inputReadingState = {true, &b, &v, joy};

    // Dirty hack to clear event queue so we don't get old remaining
    // events.  Is there a way without having to sleep?
    QThread::msleep(10);
    SDL_JoystickUpdate();
    SDL_FlushEvent(SDL_JOYAXISMOTION);
}


void InputDialog::stopReadInput()
{
    inputReadingState.button->button->setChecked(false);
    inputReadingState.button->button->setDown(false);
    SDL_JoystickClose(inputReadingState.joy);
    inputReadingState.reading = false;
    killTimer(inputTimer);
    inputTimer = 0;
}


static void setKeySpecs(std::vector<KeySpec> &keys, const KeySpec &key, int param)
{
    if (param < 0) {
        keys = {key};
    } else if (keys.empty() || keys[0].type != key.type) {
        size_t nValues = param + 1;
        if (!keys.empty() && keys[0].values.size() > nValues) {
            nValues = keys[0].values.size();
        }
        // Since param >= 0 we know there must be at least 2 values.
        if (nValues < 2) {
            nValues = 2;
        }
        keys.clear();
        keys.resize(1);
        keys[0].type = key.type;
        if (key.type == KeySpec::AXIS) {
            keys[0].values.resize(nValues, key.values[0].invertedSign());
        } else if (key.type == KeySpec::HAT) {
            keys[0].values.resize(nValues, key.values[0].invertedDirection());
        } else {
            keys[0].values.resize(nValues);
        }
        keys[0].values[param] = key.values[0];
    } else {
        keys.resize(1);
        if ((int)keys[0].values.size() <= param) {
            keys[0].values.resize(param + 1);
        }
        keys[0].values[param] = key.values[0];
    }
}


void InputDialog::timerEvent(QTimerEvent *timerEvent)
{
    SDL_JoystickID joyId;
    if (inputReadingState.reading) {
        joyId = SDL_JoystickInstanceID(inputReadingState.joy);
    } else {
        LOG_W(TR("Timer event while not reading, should never happen."));
        return;
    }
    bool gotInput = false;
    KeySpec key;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_JOYBUTTONDOWN:
            if (event.jbutton.which == joyId) {
                int n = event.jbutton.button;
                key = KeySpec(KeySpec::BUTTON, KeySpec::Value(n));
                gotInput = true;
            }
            break;
        case SDL_JOYAXISMOTION:
            if (event.jaxis.which == joyId) {
                int n = event.jaxis.axis;
                int val = event.jaxis.value;
                KeySpec::Value::Sign sign = KeySpec::Value::NO_SIGN;
                if (val > 16384) {
                    sign = KeySpec::Value::PLUS;
                } else if (val < -16384) {
                    sign = KeySpec::Value::MINUS;
                }
                if (sign != KeySpec::Value::NO_SIGN) {
                    key = KeySpec(KeySpec::AXIS, KeySpec::Value(n, sign));
                    gotInput = true;
                }
            }
            break;
        case SDL_JOYHATMOTION:
            if (event.jhat.which == joyId) {
                int n = event.jhat.hat;
                int val = event.jhat.value;
                KeySpec::Value::Direction dir;
                gotInput = true;
                switch (val) {
                case SDL_HAT_UP:    dir = KeySpec::Value::UP;    break;
                case SDL_HAT_DOWN:  dir = KeySpec::Value::DOWN;  break;
                case SDL_HAT_LEFT:  dir = KeySpec::Value::LEFT;  break;
                case SDL_HAT_RIGHT: dir = KeySpec::Value::RIGHT; break;
                default:            gotInput = false;
                }
                if (gotInput) {
                    key = KeySpec(KeySpec::HAT, KeySpec::Value(n, dir));
                }
            }
            break;
        }
    }
    if (gotInput) {
        int param = inputReadingState.button->parameter;
        std::vector<KeySpec> &keys = inputReadingState.value->keys;
        setKeySpecs(keys, key, param);
        setValues();
        currentController().changed = true;
        stopReadInput();
    }
}


static SDL_Keycode qtToSdlKey(int key)
{
    // TODO: this function
    return key;
}


void InputDialog::keyPressEvent(QKeyEvent *keyEvent)
{
    if (inputReadingState.reading) {
        int sdlKey = qtToSdlKey(keyEvent->key());
        KeySpec key = KeySpec(KeySpec::KEY, KeySpec::Value(sdlKey));
        int param = inputReadingState.button->parameter;
        std::vector<KeySpec> &keys = inputReadingState.value->keys;
        setKeySpecs(keys, key, param);
        setValues();
        currentController().changed = true;
        stopReadInput();
    } else if (keyEvent->key() == Qt::Key_Escape) {
        close();
    }
}


InputDialog::~InputDialog()
{
    if (sdlWasInited) {
        SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
    }
    delete ui;
}


InputDialog::Controller &InputDialog::currentController()
{
    return controllers[currentControllerIndex];
}


void InputDialog::getButtons()
{
    // -1 means it defines the whole keyspec, >= 0 means it only
    // defines one parameter to the keyspec.
    buttons = {
        {"X Axis",            0, ui->leftButton},
        {"X Axis",            1, ui->rightButton},
        {"Y Axis",            0, ui->upButton},
        {"Y Axis",            1, ui->downButton},
        {"A Button",         -1, ui->aButton},
        {"B Button",         -1, ui->bButton},
        {"Start",            -1, ui->startButton},
        {"L Trig",           -1, ui->lButton},
        {"R Trig",           -1, ui->rButton},
        {"Z Trig",           -1, ui->zButton},
        {"C Button U",       -1, ui->cUpButton},
        {"C Button D",       -1, ui->cDownButton},
        {"C Button L",       -1, ui->cLeftButton},
        {"C Button R",       -1, ui->cRightButton},
        {"DPad U",           -1, ui->dUpButton},
        {"DPad D",           -1, ui->dDownButton},
        {"DPad L",           -1, ui->dLeftButton},
        {"DPad R",           -1, ui->dRightButton},
        {"Mempak switch",    -1, ui->memButton},
        {"Rumblepak switch", -1, ui->rumbleButton},
    };
}



static std::vector<KeySpec> parseKeyConfig(const char *str)
{
    std::vector<KeySpec> ret;
    for (;;) {
        KeySpec k;
        if (!KeySpec::parseOne(k, &str)) {
            break;
        }
        ret.push_back(k);
    }
    return ret;
}


static QString keyspecsToString(const std::vector<KeySpec> &keyspecs)
{
    QString str;
    bool first = true;
    for (const KeySpec &k : keyspecs) {
        if (!first) {
            str += " ";
        }
        str += k.toString();
        first = false;
    }
    return str;
}


void InputDialog::setValues()
{
    Controller &c = currentController();

    // First get the values from core if we don't have them already.
    if (c.values.empty()) {
        for (const Button &b : buttons) {
            // This if statement is just to make sure we don't add the
            // same config parameter twice.
            if (b.parameter == 0 || b.parameter == -1) {
                const char *value = ConfigGetParamString(c.configHandle, b.configName);
                std::vector<KeySpec> keyspecs = parseKeyConfig(value);
                c.values.push_back(Value{b.configName, keyspecs});
            }
        }
    }

    // And then show them on the buttons.
    for (Button &b : buttons) {
        std::vector<KeySpec> keyspecs;
        // Find the value for this button.
        for (const Value &v : c.values) {
            if (strcmp(v.configName, b.configName) == 0) {
                keyspecs = v.keys;
                if (b.parameter >= 0) {
                    // Use only the specified parameter for each keyspec.
                    for (KeySpec &k : keyspecs) {
                        if ((int)k.values.size() > b.parameter) {
                            if (k.values[b.parameter].number >= 0) {
                                k.values = {k.values[b.parameter]};
                            } else {
                                keyspecs = {};
                                break;
                            }
                        } else {
                            LOG_W(TR("Parameter <N> not found in <KeySpec>.")
                                    .replace("<N>", QString::number(b.parameter))
                                    .replace("<KeySpec>", k.toString()));
                            keyspecs = {};
                            break;
                        }
                    }
                }
                break;
            }
        }
        if (keyspecs.empty()) {
            b.button->setText(TR("Select..."));
        } else {
            b.button->setText(keyspecsToString(keyspecs));
        }
    }
}


void InputDialog::saveController(int controllerIndex)
{
    const Controller &c = currentController();
    m64p_error rval;

    if (c.changed) {
        int val = 0;
        rval = ConfigSetParameter(c.configHandle, "mode", M64TYPE_INT, &val);

        for (const Value &v : c.values) {
            QString valueStr = keyspecsToString(v.keys);
            QByteArray valueBa = valueStr.toUtf8();
            const char *value = valueBa.data();
            rval = ConfigSetParameter(c.configHandle, v.configName,
                    M64TYPE_STRING, value);
            if (rval != M64ERR_SUCCESS) {
                LOG_W(TR("Could not set configuration parameter <Name>: ")
                        .replace("<Name>", v.configName) + m64errstr(rval));
            }
        }
    }

    QString sectionName = toSectionName(pluginName, controllerIndex + 1);
    rval = ConfigSaveSection(sectionName.toUtf8().data());
    if (rval != M64ERR_SUCCESS) {
        SHOW_W(TR("Could not save configuration: ") + m64errstr(rval));
    }
}


void InputDialog::accept()
{
    for (int i = 0; i < 4; i++) {
        saveController(i);
    }
    close();
}


void InputDialog::controllerSelected(int index)
{
    currentControllerIndex = index;
    setValues();
}


void InputDialog::loadUnloadPlugin(const char *name)
{
    m64p_dynlib_handle h;
    char pluginType[] = "input";
    if (openPlugin(h, name, pluginType)) {
        closePlugin(h);
    }
}
