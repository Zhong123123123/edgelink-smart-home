#include <QApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QCheckBox>
#include <QDateTime>
#include <QFrame>
#include <QColor>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QTabWidget>
#include <QRegularExpression>

#include "GatewayApiClient.h"

static QString toStr(const QVariantMap& m, const char* key, const QString& d = "-") {
    const auto it = m.find(QString::fromLatin1(key));
    return it == m.end() || it->isNull() ? d : it->toString();
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    GatewayApiClient api;
    if (argc > 1) api.setBaseUrl(QString::fromLocal8Bit(argv[1]));

    QMainWindow win;
    win.setWindowTitle("网关联调可视化面板（Qt）");
    win.resize(1360, 900);

    auto* central = new QWidget(&win);
    auto* outerLayout = new QVBoxLayout(central);

    auto* tabs = new QTabWidget();
    outerLayout->addWidget(tabs);

    // ---- Tab 0: 实时监控（原有界面） ----
    auto* monitorTab = new QWidget();
    auto* root = new QVBoxLayout(monitorTab);
    tabs->addTab(monitorTab, "实时监控");

    auto* top = new QHBoxLayout();
    auto* title = new QLabel("网关联调可视化面板");
    title->setStyleSheet("font-size:24px;font-weight:700;color:#123326;");
    auto* urlEdit = new QLineEdit(api.baseUrl());
    auto* applyBtn = new QPushButton("切换地址");
    auto* refreshBtn = new QPushButton("立即刷新");
    auto* loadingLbl = new QLabel("就绪");
    top->addWidget(title);
    top->addSpacing(16);
    top->addWidget(urlEdit, 1);
    top->addWidget(applyBtn);
    top->addWidget(refreshBtn);
    top->addWidget(loadingLbl);
    root->addLayout(top);

    auto* cards = new QGridLayout();
    QStringList cardKeys = {
        "在线设备", "解析帧总数", "WiFi客户端", "上传失败",
        "ACK接收(1分钟)", "ACK发送成功(1分钟)", "ACK发送失败(1分钟)", "ACK成功率(1分钟)"
    };
    QVector<QLabel*> cardVals;
    for (int i = 0; i < cardKeys.size(); ++i) {
        auto* box = new QWidget();
        box->setStyleSheet("background:#fff;border:1px solid #cddfd3;border-radius:8px;padding:8px;");
        auto* l = new QVBoxLayout(box);
        auto* k = new QLabel(cardKeys[i]);
        auto* v = new QLabel("-");
        v->setStyleSheet("font-size:26px;font-weight:700;color:#10291f;");
        l->addWidget(k);
        l->addWidget(v);
        cards->addWidget(box, i / 4, i % 4);
        cardVals.push_back(v);
    }
    root->addLayout(cards);

    auto* tool = new QHBoxLayout();
    auto* fAll = new QPushButton("全部设备");
    auto* f1 = new QPushButton("设备1");
    auto* f2 = new QPushButton("设备2");
    auto* onlyIssues = new QCheckBox("只看异常");
    tool->addWidget(fAll);
    tool->addWidget(f1);
    tool->addWidget(f2);
    tool->addWidget(onlyIssues);
    tool->addStretch(1);
    root->addLayout(tool);

    auto* devTitle = new QLabel("设备状态");
    devTitle->setStyleSheet("font-size:18px;font-weight:700;color:#183327;");
    root->addWidget(devTitle);

    auto* devTable = new QTableWidget(0, 8);
    devTable->setHorizontalHeaderLabels({"ID", "名称", "配置链路", "当前链路", "在线", "上报数", "RSSI", "摘要"});
    devTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    devTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    devTable->setSelectionMode(QAbstractItemView::SingleSelection);
    root->addWidget(devTable, 2);

    auto* devHint = new QLabel("提示：点击设备行，可自动联动到控制页、OTA 页和历史查询页。当前链路优先看“当前链路(active transport)”。");
    devHint->setStyleSheet("color:#6f7f76;");
    root->addWidget(devHint);

    auto* recTitle = new QLabel("最近事件（联调关键）");
    recTitle->setStyleSheet("font-size:18px;font-weight:700;color:#183327;");
    root->addWidget(recTitle);

    auto* recTable = new QTableWidget(0, 9);
    recTable->setHorizontalHeaderLabels({"设备", "Seq", "链路", "事件", "温度", "湿度", "电压", "光照", "摘要"});
    recTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    root->addWidget(recTable, 3);

    auto* logTitle = new QLabel("ACK 调试日志");
    logTitle->setStyleSheet("font-size:18px;font-weight:700;color:#183327;");
    root->addWidget(logTitle);

    auto* ackLog = new QTextEdit();
    ackLog->setReadOnly(true);
    root->addWidget(ackLog, 2);

    auto* errLbl = new QLabel();
    errLbl->setStyleSheet("color:#b23b3b;");
    root->addWidget(errLbl);

    auto* hintTitle = new QLabel("异常解释（给非技术人员）");
    hintTitle->setStyleSheet("font-size:18px;font-weight:700;color:#183327;");
    root->addWidget(hintTitle);
    auto* hintBox = new QFrame();
    hintBox->setStyleSheet("background:#fff;border:1px solid #cddfd3;border-radius:8px;padding:8px;");
    auto* hintLay = new QVBoxLayout(hintBox);
    auto* healthLight = new QLabel("● 当前链路状态：评估中");
    healthLight->setStyleSheet("font-size:16px;font-weight:700;color:#9a6b16;");
    auto* hintText = new QLabel("等待数据...");
    hintText->setWordWrap(true);
    hintText->setStyleSheet("color:#2b4a3a;");
    hintLay->addWidget(healthLight);
    hintLay->addWidget(hintText);
    root->addWidget(hintBox);

    // ---- Tab 1: 历史查询 ----
    auto* historyTab = new QWidget();
    auto* hl = new QVBoxLayout(historyTab);
    tabs->addTab(historyTab, "历史查询");

    // -- 传感器历史 --
    auto* sensorTitle = new QLabel("最近传感器历史");
    sensorTitle->setStyleSheet("font-size:18px;font-weight:700;color:#183327;");
    hl->addWidget(sensorTitle);

    auto* sensorCtrl = new QHBoxLayout();
    sensorCtrl->addWidget(new QLabel("device_id:"));
    auto* devIdSpin = new QSpinBox();
    devIdSpin->setRange(0, 255);
    devIdSpin->setValue(0);
    devIdSpin->setToolTip("0 = 全部设备");
    sensorCtrl->addWidget(devIdSpin);
    sensorCtrl->addSpacing(8);
    sensorCtrl->addWidget(new QLabel("limit:"));
    auto* sensorLimitSpin = new QSpinBox();
    sensorLimitSpin->setRange(1, 1000);
    sensorLimitSpin->setValue(20);
    sensorCtrl->addWidget(sensorLimitSpin);
    sensorCtrl->addSpacing(8);
    sensorCtrl->addWidget(new QLabel("offset:"));
    auto* sensorOffsetSpin = new QSpinBox();
    sensorOffsetSpin->setRange(0, 999999);
    sensorOffsetSpin->setValue(0);
    sensorCtrl->addWidget(sensorOffsetSpin);
    sensorCtrl->addSpacing(16);
    auto* sensorQueryBtn = new QPushButton("查询传感器历史");
    sensorCtrl->addWidget(sensorQueryBtn);
    auto* sensorStatus = new QLabel("就绪");
    sensorStatus->setStyleSheet("color:#6f7f76;");
    sensorCtrl->addWidget(sensorStatus);
    sensorCtrl->addStretch();
    hl->addLayout(sensorCtrl);

    auto* sensorErr = new QLabel();
    sensorErr->setStyleSheet("color:#b23b3b;");
    sensorErr->setWordWrap(true);
    hl->addWidget(sensorErr);

    auto* sensorTable = new QTableWidget(0, 12);
    sensorTable->setHorizontalHeaderLabels({
        "时间", "ID", "设备ID", "设备名", "帧类型",
        "温度", "湿度", "电压", "光照", "状态", "摘要", "错误"
    });
    sensorTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    hl->addWidget(sensorTable, 2);

    // -- 系统事件历史 --
    auto* sysTitle = new QLabel("最近系统事件");
    sysTitle->setStyleSheet("font-size:18px;font-weight:700;color:#183327;");
    hl->addWidget(sysTitle);

    auto* sysCtrl = new QHBoxLayout();
    sysCtrl->addWidget(new QLabel("limit:"));
    auto* sysLimitSpin = new QSpinBox();
    sysLimitSpin->setRange(1, 1000);
    sysLimitSpin->setValue(20);
    sysCtrl->addWidget(sysLimitSpin);
    sysCtrl->addSpacing(8);
    sysCtrl->addWidget(new QLabel("offset:"));
    auto* sysOffsetSpin = new QSpinBox();
    sysOffsetSpin->setRange(0, 999999);
    sysOffsetSpin->setValue(0);
    sysCtrl->addWidget(sysOffsetSpin);
    sysCtrl->addSpacing(16);
    auto* sysQueryBtn = new QPushButton("查询系统事件");
    sysCtrl->addWidget(sysQueryBtn);
    auto* sysStatus = new QLabel("就绪");
    sysStatus->setStyleSheet("color:#6f7f76;");
    sysCtrl->addWidget(sysStatus);
    sysCtrl->addStretch();
    hl->addLayout(sysCtrl);

    auto* sysErr = new QLabel();
    sysErr->setStyleSheet("color:#b23b3b;");
    sysErr->setWordWrap(true);
    hl->addWidget(sysErr);

    auto* sysTable = new QTableWidget(0, 5);
    sysTable->setHorizontalHeaderLabels({"时间", "ID", "事件类型", "设备ID", "详情"});
    sysTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    hl->addWidget(sysTable, 1);

    // ---- Tab 2: 控制台 ----
    auto* controlTab = new QWidget();
    auto* cl = new QVBoxLayout(controlTab);
    tabs->addTab(controlTab, "控制");

    auto* controlHeader = new QHBoxLayout();
    controlHeader->addWidget(new QLabel("模式:"));
    auto* controlProfileCombo = new QComboBox();
    controlProfileCombo->addItems({"MQTT 无线节点模式", "旧链路设备模式"});
    controlHeader->addWidget(controlProfileCombo);
    controlHeader->addSpacing(8);
    controlHeader->addWidget(new QLabel("device_id:"));
    auto* controlDeviceSpin = new QSpinBox();
    controlDeviceSpin->setRange(0, 255);
    controlDeviceSpin->setValue(2);
    controlHeader->addWidget(controlDeviceSpin);
    controlHeader->addSpacing(8);
    controlHeader->addWidget(new QLabel("timeout_ms:"));
    auto* controlTimeoutSpin = new QSpinBox();
    controlTimeoutSpin->setRange(100, 10000);
    controlTimeoutSpin->setValue(3000);
    controlHeader->addWidget(controlTimeoutSpin);
    controlHeader->addStretch();
    cl->addLayout(controlHeader);

    auto* controlNote = new QLabel();
    controlNote->setWordWrap(true);
    controlNote->setStyleSheet("color:#6f7f76;");
    cl->addWidget(controlNote);

    auto* cmdStateBox = new QFrame();
    cmdStateBox->setStyleSheet("background:#fff;border:1px solid #cddfd3;border-radius:8px;padding:8px;");
    auto* cmdStateLayout = new QGridLayout(cmdStateBox);
    auto* cmdStateLbl = new QLabel("idle");
    auto* cmdIdLbl = new QLabel("-");
    auto* cmdLatencyLbl = new QLabel("-");
    auto* cmdErrorLbl = new QLabel("-");
    cmdErrorLbl->setWordWrap(true);
    cmdStateLayout->addWidget(new QLabel("发送状态"), 0, 0);
    cmdStateLayout->addWidget(cmdStateLbl, 0, 1);
    cmdStateLayout->addWidget(new QLabel("command_id"), 1, 0);
    cmdStateLayout->addWidget(cmdIdLbl, 1, 1);
    cmdStateLayout->addWidget(new QLabel("latency_ms"), 2, 0);
    cmdStateLayout->addWidget(cmdLatencyLbl, 2, 1);
    cmdStateLayout->addWidget(new QLabel("error"), 3, 0);
    cmdStateLayout->addWidget(cmdErrorLbl, 3, 1);
    cl->addWidget(cmdStateBox);

    auto* mqttCmdBox = new QFrame();
    mqttCmdBox->setStyleSheet("background:#fff;border:1px solid #cddfd3;border-radius:8px;padding:8px;");
    auto* mqttCmdLayout = new QVBoxLayout(mqttCmdBox);
    auto* mqttCmdTitle = new QLabel("MQTT 无线节点命令");
    mqttCmdTitle->setStyleSheet("font-size:16px;font-weight:700;color:#183327;");
    mqttCmdLayout->addWidget(mqttCmdTitle);
    auto* mqttCmdHint = new QLabel("适用于当前 ESP32 MQTT 无线节点，推荐命令为 get_status / set_led。");
    mqttCmdHint->setWordWrap(true);
    mqttCmdHint->setStyleSheet("color:#6f7f76;");
    mqttCmdLayout->addWidget(mqttCmdHint);
    auto* cmdButtons = new QHBoxLayout();
    auto* mqttGetStatusBtn = new QPushButton("get_status");
    auto* mqttLedOnBtn = new QPushButton("LED 开");
    auto* mqttLedOffBtn = new QPushButton("LED 关");
    cmdButtons->addWidget(mqttGetStatusBtn);
    cmdButtons->addWidget(mqttLedOnBtn);
    cmdButtons->addWidget(mqttLedOffBtn);
    cmdButtons->addStretch(1);
    mqttCmdLayout->addLayout(cmdButtons);

    auto* mqttExtRow = new QHBoxLayout();
    mqttExtRow->addWidget(new QLabel("report_interval_ms:"));
    auto* reportIntervalSpin = new QSpinBox();
    reportIntervalSpin->setRange(1000, 86400000);
    reportIntervalSpin->setSingleStep(1000);
    reportIntervalSpin->setValue(3000);
    mqttExtRow->addWidget(reportIntervalSpin);
    auto* reportIntervalBtn = new QPushButton("set_report_interval");
    mqttExtRow->addWidget(reportIntervalBtn);
    mqttExtRow->addSpacing(16);
    mqttExtRow->addWidget(new QLabel("log_level:"));
    auto* logLevelCombo = new QComboBox();
    logLevelCombo->addItems({"0", "1", "2", "3", "INFO", "DEBUG", "WARN", "ERROR"});
    mqttExtRow->addWidget(logLevelCombo);
    auto* logLevelBtn = new QPushButton("set_log_level");
    mqttExtRow->addWidget(logLevelBtn);
    mqttExtRow->addStretch(1);
    mqttCmdLayout->addLayout(mqttExtRow);
    cl->addWidget(mqttCmdBox);

    auto* legacyCmdBox = new QFrame();
    legacyCmdBox->setStyleSheet("background:#fff;border:1px solid #cddfd3;border-radius:8px;padding:8px;");
    auto* legacyCmdLayout = new QVBoxLayout(legacyCmdBox);
    auto* legacyCmdTitle = new QLabel("旧链路设备命令");
    legacyCmdTitle->setStyleSheet("font-size:16px;font-weight:700;color:#183327;");
    legacyCmdLayout->addWidget(legacyCmdTitle);
    auto* legacyCmdHint = new QLabel("适用于 serial / wifi / tcp_binary 旧链路设备，可继续使用模式与阈值类命令。");
    legacyCmdHint->setWordWrap(true);
    legacyCmdHint->setStyleSheet("color:#6f7f76;");
    legacyCmdLayout->addWidget(legacyCmdHint);
    auto* legacyCmdButtons = new QHBoxLayout();
    auto* legacyGetStatusBtn = new QPushButton("get_status");
    auto* legacyLedOnBtn = new QPushButton("LED 开");
    auto* legacyLedOffBtn = new QPushButton("LED 关");
    auto* modeAutoBtn = new QPushButton("模式自动");
    auto* modeManualBtn = new QPushButton("模式手动");
    legacyCmdButtons->addWidget(legacyGetStatusBtn);
    legacyCmdButtons->addWidget(legacyLedOnBtn);
    legacyCmdButtons->addWidget(legacyLedOffBtn);
    legacyCmdButtons->addWidget(modeAutoBtn);
    legacyCmdButtons->addWidget(modeManualBtn);
    legacyCmdButtons->addStretch(1);
    legacyCmdLayout->addLayout(legacyCmdButtons);

    auto* thresholdRow = new QHBoxLayout();
    thresholdRow->addWidget(new QLabel("temperature:"));
    auto* tempSpin = new QDoubleSpinBox();
    tempSpin->setRange(-40.0, 120.0);
    tempSpin->setDecimals(1);
    tempSpin->setSingleStep(0.5);
    tempSpin->setValue(35.0);
    thresholdRow->addWidget(tempSpin);
    thresholdRow->addWidget(new QLabel("humidity:"));
    auto* humiSpin = new QDoubleSpinBox();
    humiSpin->setRange(0.0, 100.0);
    humiSpin->setDecimals(1);
    humiSpin->setSingleStep(0.5);
    humiSpin->setValue(80.0);
    thresholdRow->addWidget(humiSpin);
    auto* thresholdBtn = new QPushButton("set_threshold");
    thresholdRow->addWidget(thresholdBtn);
    thresholdRow->addStretch(1);
    legacyCmdLayout->addLayout(thresholdRow);
    cl->addWidget(legacyCmdBox);

    auto* cmdRecentTitle = new QLabel("最近命令记录");
    cmdRecentTitle->setStyleSheet("font-size:18px;font-weight:700;color:#183327;");
    cl->addWidget(cmdRecentTitle);

    auto* cmdRecentTable = new QTableWidget(0, 7);
    cmdRecentTable->setHorizontalHeaderLabels({"时间", "设备", "CommandID", "命令", "状态", "耗时", "错误"});
    cmdRecentTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    cl->addWidget(cmdRecentTable, 1);

    bool commandBusy = false;
    bool controlMqttMode = true;
    bool suppressControlProfileDefaults = false;
    auto updateCommandButtons = [&]() {
        const bool enabled = !commandBusy;
        mqttGetStatusBtn->setEnabled(enabled);
        mqttLedOnBtn->setEnabled(enabled);
        mqttLedOffBtn->setEnabled(enabled);
        reportIntervalSpin->setEnabled(enabled);
        reportIntervalBtn->setEnabled(enabled);
        logLevelCombo->setEnabled(enabled);
        logLevelBtn->setEnabled(enabled);
        legacyGetStatusBtn->setEnabled(enabled && !controlMqttMode);
        legacyLedOnBtn->setEnabled(enabled && !controlMqttMode);
        legacyLedOffBtn->setEnabled(enabled && !controlMqttMode);
        modeAutoBtn->setEnabled(enabled && !controlMqttMode);
        modeManualBtn->setEnabled(enabled && !controlMqttMode);
        thresholdBtn->setEnabled(enabled && !controlMqttMode);
        controlDeviceSpin->setEnabled(enabled);
        controlTimeoutSpin->setEnabled(enabled);
        controlProfileCombo->setEnabled(enabled);
        tempSpin->setEnabled(enabled && !controlMqttMode);
        humiSpin->setEnabled(enabled && !controlMqttMode);
    };
    auto applyControlProfile = [&](bool applyDefaults) {
        controlMqttMode = (controlProfileCombo->currentIndex() == 0);
        if (controlMqttMode) {
            if (applyDefaults) {
                controlDeviceSpin->setValue(2);
                controlTimeoutSpin->setValue(3000);
            }
            controlNote->setText("当前为 MQTT 无线节点模式。默认设备为 device 2，建议使用 get_status / set_led；set_mode / set_threshold 在该模式下不作为主链路命令。");
            mqttCmdBox->show();
            legacyCmdBox->hide();
        } else {
            if (applyDefaults) {
                controlDeviceSpin->setValue(1);
                controlTimeoutSpin->setValue(3000);
            }
            controlNote->setText("当前为旧链路设备模式。适用于 serial / wifi / tcp_binary 旧设备，支持 get_status / set_led / set_mode / set_threshold。");
            mqttCmdBox->hide();
            legacyCmdBox->show();
        }
        updateCommandButtons();
    };
    auto setCommandBusy = [&](bool busy) {
        commandBusy = busy;
        updateCommandButtons();
        if (busy) {
            cmdStateLbl->setText("sending");
            cmdErrorLbl->setText("-");
        }
    };
    auto sendControlCommand = [&](const QString& commandType, const QVariantMap& args) {
        if (commandBusy) return;
        QVariantMap body;
        body.insert("device_id", controlDeviceSpin->value());
        body.insert("command_type", commandType);
        for (auto it = args.begin(); it != args.end(); ++it) {
            body.insert(it.key(), it.value());
        }
        body.insert("timeout_ms", controlTimeoutSpin->value());
        setCommandBusy(true);
        cmdStateLbl->setText("sending");
        cmdIdLbl->setText("-");
        cmdLatencyLbl->setText("-");
        cmdErrorLbl->setText("-");
        api.postJson("/api/command", body, controlTimeoutSpin->value());
    };

    QObject::connect(mqttGetStatusBtn, &QPushButton::clicked, [&]() {
        sendControlCommand("get_status", QVariantMap{});
    });
    QObject::connect(mqttLedOnBtn, &QPushButton::clicked, [&]() {
        sendControlCommand("set_led", QVariantMap{{"on", true}});
    });
    QObject::connect(mqttLedOffBtn, &QPushButton::clicked, [&]() {
        sendControlCommand("set_led", QVariantMap{{"on", false}});
    });
    QObject::connect(reportIntervalBtn, &QPushButton::clicked, [&]() {
        sendControlCommand("set_report_interval", QVariantMap{{"interval_ms", reportIntervalSpin->value()}});
    });
    QObject::connect(logLevelBtn, &QPushButton::clicked, [&]() {
        sendControlCommand("set_log_level", QVariantMap{{"level", logLevelCombo->currentText()}});
    });
    QObject::connect(legacyGetStatusBtn, &QPushButton::clicked, [&]() {
        sendControlCommand("get_status", QVariantMap{});
    });
    QObject::connect(legacyLedOnBtn, &QPushButton::clicked, [&]() {
        sendControlCommand("set_led", QVariantMap{{"on", true}});
    });
    QObject::connect(legacyLedOffBtn, &QPushButton::clicked, [&]() {
        sendControlCommand("set_led", QVariantMap{{"on", false}});
    });
    QObject::connect(modeAutoBtn, &QPushButton::clicked, [&]() {
        sendControlCommand("set_mode", QVariantMap{{"mode", "auto"}});
    });
    QObject::connect(modeManualBtn, &QPushButton::clicked, [&]() {
        sendControlCommand("set_mode", QVariantMap{{"mode", "manual"}});
    });
    QObject::connect(thresholdBtn, &QPushButton::clicked, [&]() {
        sendControlCommand("set_threshold", QVariantMap{{"temperature", tempSpin->value()}, {"humidity", humiSpin->value()}});
    });
    QObject::connect(controlProfileCombo, &QComboBox::currentIndexChanged, [&]() {
        const bool applyDefaults = !suppressControlProfileDefaults;
        applyControlProfile(applyDefaults);
    });
    applyControlProfile(true);

    // ---- Tab 3: OTA ----
    auto* otaTab = new QWidget();
    auto* ol = new QVBoxLayout(otaTab);
    tabs->addTab(otaTab, "OTA");

    auto* otaWarn = new QLabel("危险操作：OTA 需要二次确认，先确认设备型号、固件 ID 和传输方式再发起。");
    otaWarn->setStyleSheet("color:#9a6b16;font-weight:700;");
    otaWarn->setWordWrap(true);
    ol->addWidget(otaWarn);

    auto* otaForm = new QGridLayout();
    auto* otaProfileCombo = new QComboBox();
    otaProfileCombo->addItems({"MQTT 无线节点模式", "旧链路设备模式"});
    auto* otaDeviceSpin = new QSpinBox();
    otaDeviceSpin->setRange(0, 255);
    otaDeviceSpin->setValue(2);
    auto* otaDeviceTypeEdit = new QLineEdit("esp32-wifi-node");
    auto* otaFirmwareIdEdit = new QLineEdit("esp32-wifi-node-1.1.0");
    auto* otaTransportCombo = new QComboBox();
    otaTransportCombo->addItems({"mqtt", "serial", "tcp_binary"});
    auto* otaTargetEdit = new QLineEdit("127.0.0.1:19090");
    auto* otaFirmwareUrlEdit = new QLineEdit("http://example.com/fake.bin");
    otaFirmwareUrlEdit->setPlaceholderText("ESP32 OTA 可选覆盖 firmware_url");
    auto* otaConfirmCheck = new QCheckBox("我已确认：这是 OTA 危险操作");
    auto* otaCreateBtn = new QPushButton("创建 OTA 任务");
    auto* otaRefreshBtn = new QPushButton("刷新任务");

    otaForm->addWidget(new QLabel("模式"), 0, 0);
    otaForm->addWidget(otaProfileCombo, 0, 1);
    otaForm->addWidget(new QLabel("device_id"), 0, 2);
    otaForm->addWidget(otaDeviceSpin, 0, 3);
    otaForm->addWidget(new QLabel("device_type"), 1, 0);
    otaForm->addWidget(otaDeviceTypeEdit, 1, 1);
    otaForm->addWidget(new QLabel("firmware_id"), 1, 2);
    otaForm->addWidget(otaFirmwareIdEdit, 1, 3);
    otaForm->addWidget(new QLabel("transport"), 2, 0);
    otaForm->addWidget(otaTransportCombo, 2, 1);
    otaForm->addWidget(new QLabel("target"), 2, 2);
    otaForm->addWidget(otaTargetEdit, 2, 3);
    otaForm->addWidget(new QLabel("firmware_url"), 3, 0);
    otaForm->addWidget(otaFirmwareUrlEdit, 3, 1, 1, 3);
    otaForm->addWidget(otaConfirmCheck, 4, 0, 1, 2);
    otaForm->addWidget(otaCreateBtn, 4, 2);
    otaForm->addWidget(otaRefreshBtn, 4, 3);
    ol->addLayout(otaForm);

    auto* otaNote = new QLabel();
    otaNote->setWordWrap(true);
    otaNote->setStyleSheet("color:#6f7f76;");
    ol->addWidget(otaNote);

    auto* otaStateLbl = new QLabel("就绪");
    otaStateLbl->setStyleSheet("color:#6f7f76;");
    ol->addWidget(otaStateLbl);

    auto* otaTopSplit = new QHBoxLayout();
    auto* otaTaskTable = new QTableWidget(0, 7);
    otaTaskTable->setHorizontalHeaderLabels({"Task UUID", "设备", "类型", "固件", "状态", "进度", "错误"});
    otaTaskTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    otaTaskTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    otaTaskTable->setSelectionMode(QAbstractItemView::SingleSelection);
    otaTopSplit->addWidget(otaTaskTable, 3);

    auto* otaDetailBox = new QVBoxLayout();
    auto* otaDetailTitle = new QLabel("选中任务详情");
    otaDetailTitle->setStyleSheet("font-size:18px;font-weight:700;color:#183327;");
    auto* otaDetailState = new QLabel("state: -");
    auto* otaDetailProgress = new QLabel("progress: -");
    auto* otaDetailError = new QLabel("error: -");
    otaDetailError->setWordWrap(true);
    auto* otaDetailEvents = new QPlainTextEdit();
    otaDetailEvents->setReadOnly(true);
    otaDetailBox->addWidget(otaDetailTitle);
    otaDetailBox->addWidget(otaDetailState);
    otaDetailBox->addWidget(otaDetailProgress);
    otaDetailBox->addWidget(otaDetailError);
    otaDetailBox->addWidget(new QLabel("事件记录"));
    otaDetailBox->addWidget(otaDetailEvents, 1);
    otaTopSplit->addLayout(otaDetailBox, 2);
    ol->addLayout(otaTopSplit, 1);

    auto* otaActionRow = new QHBoxLayout();
    auto* otaCancelBtn = new QPushButton("取消选中任务");
    auto* otaRetryBtn = new QPushButton("重试选中任务");
    otaActionRow->addWidget(otaCancelBtn);
    otaActionRow->addWidget(otaRetryBtn);
    otaActionRow->addStretch(1);
    ol->addLayout(otaActionRow);

    auto parseProgress = [](const QString& text) -> QString {
        const auto rx = QRegularExpression(R"(progress=(\d+))");
        const auto m = rx.match(text);
        if (m.hasMatch()) return m.captured(1) + "%";
        const auto rx2 = QRegularExpression(R"(pct=(\d+))");
        const auto m2 = rx2.match(text);
        if (m2.hasMatch()) return m2.captured(1) + "%";
        return "-";
    };

    auto stateColor = [](const QString& state) -> QColor {
        const auto s = state.trimmed().toUpper();
        if (s == "SUCCESS") return QColor("#1f8f5f");
        if (s == "FAILED") return QColor("#b23b3b");
        if (s == "CANCELED") return QColor("#6f7f76");
        if (s == "ROLLBACK") return QColor("#8b5a00");
        if (s == "TRANSFERRING" || s == "PREPARE_DEVICE" || s == "VERIFYING" || s == "COMMITTING" ||
            s == "REBOOTING" || s == "WAIT_DEVICE_ONLINE" || s == "PRECHECK") {
            return QColor("#9a6b16");
        }
        return QColor("#183327");
    };

    auto applyOtaStateStyle = [&](const QString& state) {
        const QColor c = stateColor(state);
        otaDetailState->setStyleSheet(QString("font-size:16px;font-weight:700;color:%1;").arg(c.name()));
    };
    bool suppressOtaProfileDefaults = false;
    auto applyOtaProfile = [&](bool applyDefaults) {
        const bool mqttMode = (otaProfileCombo->currentIndex() == 0);
        if (mqttMode) {
            if (applyDefaults) {
                otaDeviceSpin->setValue(2);
                otaDeviceTypeEdit->setText("esp32-wifi-node");
                otaFirmwareIdEdit->setText("esp32-wifi-node-1.1.0");
                otaTransportCombo->setCurrentText("mqtt");
                otaTargetEdit->setText("127.0.0.1:19090");
            }
            otaTargetEdit->setEnabled(false);
            if (applyDefaults && otaFirmwareUrlEdit->text().trimmed().isEmpty()) {
                otaFirmwareUrlEdit->setText("http://example.com/fake.bin");
            }
            otaFirmwareUrlEdit->setEnabled(true);
            otaNote->setText("当前为 MQTT 无线节点模式。默认对齐 device 2 / esp32-wifi-node / transport=mqtt，firmware_url 可按需覆盖。");
        } else {
            if (applyDefaults) {
                otaDeviceSpin->setValue(1);
                otaDeviceTypeEdit->setText("stm32f407-smarthome");
                otaFirmwareIdEdit->setText("stm32f407-smarthome-1.3.9-a1");
                otaTransportCombo->setCurrentText("tcp_binary");
                otaTargetEdit->setText("127.0.0.1:19090");
            }
            otaTargetEdit->setEnabled(true);
            if (applyDefaults) {
                otaFirmwareUrlEdit->clear();
            }
            otaFirmwareUrlEdit->setEnabled(false);
            otaNote->setText("当前为旧链路设备模式。默认对齐 STM32 旧链路 OTA；常用 transport 为 tcp_binary，target 需要按现场链路确认。");
        }
    };

    QString selectedOtaTaskUuid;
    auto refreshSelectedOtaTask = [&]() {
        if (selectedOtaTaskUuid.isEmpty()) {
            otaDetailState->setText("state: -");
            otaDetailProgress->setText("progress: -");
            otaDetailError->setText("error: -");
            otaDetailState->setStyleSheet("font-size:16px;font-weight:700;color:#183327;");
            otaDetailEvents->clear();
            return;
        }
        api.fetchOtaTaskEvents(selectedOtaTaskUuid);
    };

    QObject::connect(otaRefreshBtn, &QPushButton::clicked, [&]() {
        otaStateLbl->setText("刷新 OTA 任务中...");
        api.refreshOtaTasks();
        refreshSelectedOtaTask();
    });
    QObject::connect(otaProfileCombo, &QComboBox::currentIndexChanged, [&]() {
        const bool applyDefaults = !suppressOtaProfileDefaults;
        applyOtaProfile(applyDefaults);
    });
    QObject::connect(otaConfirmCheck, &QCheckBox::toggled, [&](bool checked) {
        otaCreateBtn->setEnabled(checked);
    });
    otaCreateBtn->setEnabled(false);
    applyOtaProfile(true);

    auto syncUiToDevice = [&](int deviceId, const QString& linkType, const QString& deviceName) {
        const bool mqttMode = (linkType.trimmed().compare("mqtt", Qt::CaseInsensitive) == 0);

        suppressControlProfileDefaults = true;
        controlProfileCombo->setCurrentIndex(mqttMode ? 0 : 1);
        suppressControlProfileDefaults = false;
        controlDeviceSpin->setValue(deviceId);

        suppressOtaProfileDefaults = true;
        otaProfileCombo->setCurrentIndex(mqttMode ? 0 : 1);
        suppressOtaProfileDefaults = false;
        otaDeviceSpin->setValue(deviceId);

        devIdSpin->setValue(deviceId);

        if (mqttMode) {
            if (deviceName.contains("esp32", Qt::CaseInsensitive) ||
                deviceName.contains("wifi-node", Qt::CaseInsensitive)) {
                if (otaDeviceTypeEdit->text().trimmed().isEmpty()) {
                    otaDeviceTypeEdit->setText("esp32-wifi-node");
                }
            }
        }

        cmdStateLbl->setText(QString("selected device %1").arg(deviceId));
        cmdErrorLbl->setText(QString("link_type=%1").arg(linkType));
        otaStateLbl->setText(QString("已选中 device %1 (%2)").arg(deviceId).arg(linkType));
    };

    int selectedDeviceId = -1;

    QObject::connect(devTable, &QTableWidget::itemSelectionChanged, [&]() {
        const auto items = devTable->selectedItems();
        if (items.isEmpty()) return;
        const int row = items.first()->row();
        auto* idItem = devTable->item(row, 0);
        auto* nameItem = devTable->item(row, 1);
        auto* linkItem = devTable->item(row, 2);
        if (idItem == nullptr || linkItem == nullptr) return;
        selectedDeviceId = idItem->text().toInt();
        syncUiToDevice(selectedDeviceId, linkItem->text(), nameItem ? nameItem->text() : QString());
    });

    auto createOtaTask = [&]() {
        if (!otaConfirmCheck->isChecked()) {
            otaStateLbl->setText("请先勾选二次确认");
            return;
        }
        const auto ret = QMessageBox::warning(
            &win,
            "确认 OTA",
            "即将创建 OTA 任务。请确认设备、固件和传输方式无误。",
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (ret != QMessageBox::Yes) {
            otaStateLbl->setText("已取消");
            return;
        }

        QVariantMap body;
        body.insert("device_id", otaDeviceSpin->value());
        body.insert("device_type", otaDeviceTypeEdit->text().trimmed());
        body.insert("firmware_id", otaFirmwareIdEdit->text().trimmed());
        body.insert("transport", otaTransportCombo->currentText());
        body.insert("target", otaTargetEdit->text().trimmed());
        if (body.value("device_type").toString().isEmpty() ||
            body.value("firmware_id").toString().isEmpty()) {
            otaStateLbl->setText("device_type / firmware_id 不能为空");
            return;
        }
        if (body.value("transport").toString() != "mqtt" &&
            body.value("target").toString().isEmpty()) {
            otaStateLbl->setText("非 MQTT OTA 必须填写 target");
            return;
        }
        if (!otaFirmwareUrlEdit->text().trimmed().isEmpty()) {
            body.insert("firmware_url", otaFirmwareUrlEdit->text().trimmed());
        }
        otaStateLbl->setText("创建 OTA 任务中...");
        api.postJson("/api/ota/tasks", body, 5000);
    };
    QObject::connect(otaCreateBtn, &QPushButton::clicked, createOtaTask);
    QObject::connect(otaCancelBtn, &QPushButton::clicked, [&]() {
        const auto item = otaTaskTable->currentItem();
        if (item == nullptr) {
            otaStateLbl->setText("请先选中一个任务");
            return;
        }
        const int row = item->row();
        const auto uuidItem = otaTaskTable->item(row, 0);
        if (uuidItem == nullptr) return;
        const QString taskUuid = uuidItem->text().trimmed();
        if (taskUuid.isEmpty()) return;
        const auto ret = QMessageBox::warning(
            &win,
            "确认取消",
            QString("确认取消任务 %1 ?").arg(taskUuid),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (ret != QMessageBox::Yes) return;
        otaStateLbl->setText("取消任务中...");
        api.postJson(QString("/api/ota/tasks/%1/cancel").arg(taskUuid), QVariantMap{}, 5000);
    });
    QObject::connect(otaRetryBtn, &QPushButton::clicked, [&]() {
        const auto item = otaTaskTable->currentItem();
        if (item == nullptr) {
            otaStateLbl->setText("请先选中一个任务");
            return;
        }
        const int row = item->row();
        const auto uuidItem = otaTaskTable->item(row, 0);
        if (uuidItem == nullptr) return;
        const QString taskUuid = uuidItem->text().trimmed();
        if (taskUuid.isEmpty()) return;
        const auto ret = QMessageBox::warning(
            &win,
            "确认重试",
            QString("确认重试任务 %1 ?").arg(taskUuid),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (ret != QMessageBox::Yes) return;
        otaStateLbl->setText("重试任务中...");
        api.postJson(QString("/api/ota/tasks/%1/retry").arg(taskUuid), QVariantMap{}, 5000);
    });
    QObject::connect(otaTaskTable, &QTableWidget::itemSelectionChanged, [&]() {
        const auto items = otaTaskTable->selectedItems();
        if (items.isEmpty()) {
            selectedOtaTaskUuid.clear();
            refreshSelectedOtaTask();
            return;
        }
        selectedOtaTaskUuid = otaTaskTable->item(items.first()->row(), 0)->text().trimmed();
        refreshSelectedOtaTask();
    });

    QObject::connect(&api, &GatewayApiClient::otaTasksUpdated, [&]() {
        const auto tasks = api.otaTasks();
        otaTaskTable->setRowCount(0);
        for (const auto& v : tasks) {
            const auto m = v.toMap();
            const int r = otaTaskTable->rowCount();
            otaTaskTable->insertRow(r);
            const QString state = m.value("state").toString();
            const QString progress = parseProgress(m.value("last_error").toString());
            otaTaskTable->setItem(r, 0, new QTableWidgetItem(m.value("task_uuid").toString()));
            otaTaskTable->setItem(r, 1, new QTableWidgetItem(QString::number(m.value("device_id").toInt())));
            otaTaskTable->setItem(r, 2, new QTableWidgetItem(m.value("device_type").toString()));
            otaTaskTable->setItem(r, 3, new QTableWidgetItem(m.value("firmware_id").toString()));
            auto* stateItem = new QTableWidgetItem(state);
            stateItem->setForeground(stateColor(state));
            otaTaskTable->setItem(r, 4, stateItem);
            otaTaskTable->setItem(r, 5, new QTableWidgetItem(progress));
            otaTaskTable->setItem(r, 6, new QTableWidgetItem(m.value("last_error").toString()));
        }
        otaStateLbl->setText(QString("共 %1 个任务").arg(tasks.size()));
        if (!selectedOtaTaskUuid.isEmpty()) {
            for (int row = 0; row < otaTaskTable->rowCount(); ++row) {
                const auto* uuidItem = otaTaskTable->item(row, 0);
                if (uuidItem != nullptr && uuidItem->text().trimmed() == selectedOtaTaskUuid) {
                    otaTaskTable->selectRow(row);
                    break;
                }
            }
        }
    });

    QObject::connect(&api, &GatewayApiClient::otaTaskEventsUpdated, [&]() {
        const auto events = api.otaTaskEvents();
        otaDetailEvents->clear();
        QString stateText = "state: -";
        QString progressText = "progress: -";
        QString errorText = "error: -";
        QStringList lines;
        for (const auto& v : events) {
            const auto m = v.toMap();
            const QString ts = QDateTime::fromMSecsSinceEpoch(m.value("ts_unix_ms").toULongLong()).toString("yyyy-MM-dd HH:mm:ss");
            const QString state = m.value("state").toString();
            const QString detail = m.value("detail").toString();
            lines << QString("%1 | %2 | %3").arg(ts, state, detail);
            stateText = "state: " + state;
            const auto p = parseProgress(detail);
            if (p != "-") {
                progressText = "progress: " + p;
            }
            if (!detail.isEmpty()) {
                errorText = "error: " + detail;
            }
        }
        otaDetailEvents->setPlainText(lines.join('\n'));
        otaDetailState->setText(stateText);
        applyOtaStateStyle(stateText.mid(7));
        otaDetailProgress->setText(progressText);
        otaDetailError->setText(errorText);
    });

    otaRefreshBtn->click();

    win.setCentralWidget(central);

    int deviceFilter = -1;

    QObject::connect(fAll, &QPushButton::clicked, [&]() { deviceFilter = -1; });
    QObject::connect(f1, &QPushButton::clicked, [&]() { deviceFilter = 1; });
    QObject::connect(f2, &QPushButton::clicked, [&]() { deviceFilter = 2; });
    QObject::connect(applyBtn, &QPushButton::clicked, [&]() {
        api.setBaseUrl(urlEdit->text());
        api.refreshNow();
    });
    QObject::connect(refreshBtn, &QPushButton::clicked, &api, &GatewayApiClient::refreshNow);

    QObject::connect(&api, &GatewayApiClient::loadingChanged, [&]() {
        loadingLbl->setText(api.loading() ? "正在刷新..." : "实时刷新：1秒");
    });
    QObject::connect(&api, &GatewayApiClient::lastErrorChanged, [&]() {
        errLbl->setText(api.lastError().isEmpty() ? "" : ("错误: " + api.lastError()));
    });

    QObject::connect(&api, &GatewayApiClient::dataUpdated, [&]() {
        const auto st = api.status();
        const auto ack = api.ackDebug();
        cardVals[0]->setText(toStr(st, "devices_online", "0"));
        cardVals[1]->setText(toStr(st, "parsed_ok", "0"));
        cardVals[2]->setText(toStr(st, "wifi_clients_connected", "0"));
        cardVals[3]->setText(toStr(st, "upload_failed", "0"));
        cardVals[4]->setText(QString::number(ack.value("rx_1m", 0).toInt()));
        cardVals[5]->setText(QString::number(ack.value("tx_ok_1m", 0).toInt()));
        cardVals[6]->setText(QString::number(ack.value("tx_fail_1m", 0).toInt()));
        cardVals[7]->setText(QString::number(ack.value("ack_success_rate_1m", 0.0).toDouble(), 'f', 1) + "%");

        const int ackRx1m = ack.value("rx_1m", 0).toInt();
        const int ackFail1m = ack.value("tx_fail_1m", 0).toInt();
        const double ackSucc = ack.value("ack_success_rate_1m", 0.0).toDouble();
        const int online = st.value("devices_online", 0).toInt();
        const int offline = st.value("devices_offline", 0).toInt();
        if (ackRx1m == 0 && online == 0) {
            healthLight->setText("● 当前链路状态：待机");
            healthLight->setStyleSheet("font-size:16px;font-weight:700;color:#6f7f76;");
            hintText->setText("当前没有设备在上报。若现场设备已上电，请检查网关地址、WiFi 和接线。");
        } else if (ackSucc >= 98.0 && ackFail1m == 0) {
            healthLight->setText("● 当前链路状态：正常");
            healthLight->setStyleSheet("font-size:16px;font-weight:700;color:#1f8f5f;");
            hintText->setText("数据链路稳定，ACK 回包正常。可继续观察温湿度、电压和设备在线情况。");
        } else if (ackSucc >= 90.0) {
            healthLight->setText("● 当前链路状态：轻微波动");
            healthLight->setStyleSheet("font-size:16px;font-weight:700;color:#9a6b16;");
            hintText->setText("链路偶有丢包或延迟。建议观察 ACK 失败是否持续增加，并关注离线设备数量。");
        } else {
            healthLight->setText("● 当前链路状态：异常");
            healthLight->setStyleSheet("font-size:16px;font-weight:700;color:#b23b3b;");
            hintText->setText("ACK 成功率偏低，可能是网关回包通道异常、网络抖动或设备侧超时。优先查看下方 ACK 调试日志。");
        }
        if (offline > 0) {
            hintText->setText(hintText->text() + QString(" 当前离线设备: %1。").arg(offline));
        }

        const auto devices = api.devices();
        devTable->setRowCount(0);
        for (const auto& v : devices) {
            const auto m = v.toMap();
            const int did = m.value("device_id").toInt();
            if (deviceFilter >= 0 && did != deviceFilter) continue;
            int r = devTable->rowCount();
            devTable->insertRow(r);
            devTable->setItem(r, 0, new QTableWidgetItem(QString::number(did)));
            devTable->setItem(r, 1, new QTableWidgetItem(m.value("device_name").toString()));
            devTable->setItem(r, 2, new QTableWidgetItem(m.value("link_type").toString()));
            const QString activeTransport = m.value("active_transport").toString();
            auto* activeItem = new QTableWidgetItem(activeTransport.isEmpty() ? "-" : activeTransport);
            if (activeTransport.compare("mqtt", Qt::CaseInsensitive) == 0) {
                activeItem->setForeground(QColor("#1f8f5f"));
            } else if (!activeTransport.isEmpty()) {
                activeItem->setForeground(QColor("#9a6b16"));
            }
            devTable->setItem(r, 3, activeItem);
            devTable->setItem(r, 4, new QTableWidgetItem(m.value("online").toBool() ? "在线" : "离线"));
            devTable->setItem(r, 5, new QTableWidgetItem(QString::number(m.value("report_count").toInt())));
            devTable->setItem(r, 6, new QTableWidgetItem(QString::number(m.value("wifi_rssi").toInt())));
            devTable->setItem(r, 7, new QTableWidgetItem(m.value("last_summary").toString()));
            if (did == selectedDeviceId) {
                devTable->selectRow(r);
            }
        }

        auto isIssue = [](const QVariantMap& m) {
            const QString evt = m.value("event_type").toString();
            const QString summary = m.value("payload_summary").toString().toLower();
            return evt == "unknown" || evt == "command_ack" || summary.contains("fail") || summary.contains("timeout") || summary.contains("unknown");
        };

        const auto recent = api.recent();
        recTable->setRowCount(0);
        for (int i = recent.size() - 1; i >= 0; --i) {
            const auto m = recent[i].toMap();
            const int did = m.value("device_id").toInt();
            if (deviceFilter >= 0 && did != deviceFilter) continue;
            if (onlyIssues->isChecked() && !isIssue(m)) continue;
            int r = recTable->rowCount();
            recTable->insertRow(r);
            recTable->setItem(r, 0, new QTableWidgetItem(QString::number(did)));
            recTable->setItem(r, 1, new QTableWidgetItem(QString::number(m.value("seq").toULongLong())));
            recTable->setItem(r, 2, new QTableWidgetItem(m.value("link_type").toString()));
            recTable->setItem(r, 3, new QTableWidgetItem(m.value("event_type").toString()));
            recTable->setItem(r, 4, new QTableWidgetItem(QString::number(m.value("temperature").toDouble(), 'f', 1) + " °C"));
            recTable->setItem(r, 5, new QTableWidgetItem(QString::number(m.value("humidity").toDouble(), 'f', 1) + " %"));
            recTable->setItem(r, 6, new QTableWidgetItem(QString::number(m.value("voltage").toDouble(), 'f', 2) + " V"));
            const double light = m.value("light").toDouble();
            recTable->setItem(r, 7, new QTableWidgetItem(light < 0.0 ? "-" : (QString::number(light, 'f', 0) + " lux")));
            recTable->setItem(r, 8, new QTableWidgetItem(m.value("payload_summary").toString()));
            if (isIssue(m)) {
                for (int c = 0; c < recTable->columnCount(); ++c) {
                    if (auto* it = recTable->item(r, c)) it->setBackground(QColor("#ffecec"));
                }
            }
            if (r > 150) break;
        }

        QStringList lines;
        for (const auto& lv : ack.value("lines").toList()) lines.push_back(lv.toString());
        ackLog->setPlainText(lines.join("\n"));

        const auto commandRecent = api.commandRecent();
        cmdRecentTable->setRowCount(0);
        for (const auto& v : commandRecent) {
            const auto m = v.toMap();
            int r = cmdRecentTable->rowCount();
            cmdRecentTable->insertRow(r);
            cmdRecentTable->setItem(r, 0, new QTableWidgetItem(m.value("time").toString()));
            cmdRecentTable->setItem(r, 1, new QTableWidgetItem(QString::number(m.value("device_id").toInt())));
            cmdRecentTable->setItem(r, 2, new QTableWidgetItem(QString::number(m.value("command_id").toInt())));
            cmdRecentTable->setItem(r, 3, new QTableWidgetItem(m.value("command_type").toString()));
            cmdRecentTable->setItem(r, 4, new QTableWidgetItem(m.value("status").toString()));
            cmdRecentTable->setItem(r, 5, new QTableWidgetItem(QString::number(m.value("latency_ms").toInt())));
            cmdRecentTable->setItem(r, 6, new QTableWidgetItem(m.value("error").toString()));
        }
    });

    // -- 历史查询按钮点击 --
    QObject::connect(sensorQueryBtn, &QPushButton::clicked, [&]() {
        sensorStatus->setText("查询中...");
        api.fetchHistory(devIdSpin->value(), sensorLimitSpin->value(), sensorOffsetSpin->value());
    });
    QObject::connect(sysQueryBtn, &QPushButton::clicked, [&]() {
        sysStatus->setText("查询中...");
        api.fetchSystemHistory(sysLimitSpin->value(), sysOffsetSpin->value());
    });

    // -- 历史数据更新 --
    QObject::connect(&api, &GatewayApiClient::historyUpdated, [&]() {
        // 传感器历史
        const auto hdr = api.historyEvents();
        sensorTable->setRowCount(0);
        sensorErr->setText("");
        if (hdr.isEmpty()) {
            sensorStatus->setText("暂无历史数据");
        } else {
            sensorStatus->setText(QString("共 %1 条").arg(hdr.size()));
            for (const auto& v : hdr) {
                const auto m = v.toMap();
                int r = sensorTable->rowCount();
                sensorTable->insertRow(r);
                const qint64 ts = m.value("ts_unix_ms").toLongLong();
                const QString tsStr = ts > 0
                    ? QDateTime::fromMSecsSinceEpoch(ts).toString("yyyy-MM-dd HH:mm:ss")
                    : "-";
                sensorTable->setItem(r, 0, new QTableWidgetItem(tsStr));
                sensorTable->setItem(r, 1, new QTableWidgetItem(QString::number(m.value("id").toLongLong())));
                sensorTable->setItem(r, 2, new QTableWidgetItem(QString::number(m.value("device_id").toInt())));
                sensorTable->setItem(r, 3, new QTableWidgetItem(m.value("device_name").toString()));
                sensorTable->setItem(r, 4, new QTableWidgetItem(m.value("frame_type").toString()));
                sensorTable->setItem(r, 5, new QTableWidgetItem(QString::number(m.value("temperature").toDouble(), 'f', 1)));
                sensorTable->setItem(r, 6, new QTableWidgetItem(QString::number(m.value("humidity").toDouble(), 'f', 1)));
                sensorTable->setItem(r, 7, new QTableWidgetItem(QString::number(m.value("voltage").toDouble(), 'f', 2)));
                const double light = m.value("light").toDouble();
                sensorTable->setItem(r, 8, new QTableWidgetItem(light < 0.0 ? "-" : QString::number(light, 'f', 0)));
                sensorTable->setItem(r, 9, new QTableWidgetItem(QString::number(m.value("status").toInt())));
                sensorTable->setItem(r, 10, new QTableWidgetItem(m.value("payload_summary").toString()));
                sensorTable->setItem(r, 11, new QTableWidgetItem(m.value("last_error").toString()));
            }
        }
        if (!api.historyError().isEmpty()) {
            sensorErr->setText("错误: " + api.historyError());
            sensorStatus->setText("查询失败");
        }

        // 系统事件
        const auto sys = api.systemEvents();
        sysTable->setRowCount(0);
        sysErr->setText("");
        if (sys.isEmpty()) {
            sysStatus->setText("暂无历史数据");
        } else {
            sysStatus->setText(QString("共 %1 条").arg(sys.size()));
            for (const auto& v : sys) {
                const auto m = v.toMap();
                int r = sysTable->rowCount();
                sysTable->insertRow(r);
                const qint64 ts = m.value("ts_unix_ms").toLongLong();
                const QString tsStr = ts > 0
                    ? QDateTime::fromMSecsSinceEpoch(ts).toString("yyyy-MM-dd HH:mm:ss")
                    : "-";
                sysTable->setItem(r, 0, new QTableWidgetItem(tsStr));
                sysTable->setItem(r, 1, new QTableWidgetItem(QString::number(m.value("id").toLongLong())));
                sysTable->setItem(r, 2, new QTableWidgetItem(m.value("event_type").toString()));
                sysTable->setItem(r, 3, new QTableWidgetItem(QString::number(m.value("device_id").toInt())));
                sysTable->setItem(r, 4, new QTableWidgetItem(m.value("detail").toString()));
            }
        }
        if (!api.systemHistoryError().isEmpty()) {
            sysErr->setText("错误: " + api.systemHistoryError());
            sysStatus->setText("查询失败");
        }
    });

    QObject::connect(&api, &GatewayApiClient::jsonResponseReceived,
                     [&](const QString& path, int httpStatus, const QVariantMap& response, const QString& error) {
        if (path != "/api/command") {
            if (path == "/api/ota/tasks" ||
                path.endsWith("/cancel") ||
                path.endsWith("/retry")) {
                if (!error.isEmpty() && response.isEmpty()) {
                    otaStateLbl->setText("failed: " + error);
                } else if (!response.isEmpty()) {
                    const auto taskUuid = response.value("task_uuid").toString();
                    const auto ok = response.value("ok").toBool();
                    const auto state = response.value("state").toString();
                    const auto errText = response.value("error").toString();
                    if (!taskUuid.isEmpty()) {
                        otaStateLbl->setText(ok ? (state.isEmpty() ? "ok" : state)
                                                : (state.isEmpty() ? "failed" : state));
                    } else if (ok) {
                        otaStateLbl->setText("ok");
                    }
                    if (!errText.isEmpty()) {
                        otaStateLbl->setText(otaStateLbl->text() + ": " + errText);
                    }
                }
                api.refreshOtaTasks();
                if (!selectedOtaTaskUuid.isEmpty()) {
                    refreshSelectedOtaTask();
                }
            }
            return;
        }
        setCommandBusy(false);

        if (!error.isEmpty() && response.isEmpty()) {
            cmdStateLbl->setText("failed");
            cmdErrorLbl->setText(error);
            cmdIdLbl->setText("-");
            cmdLatencyLbl->setText("-");
            return;
        }

        const bool ok = response.value("ok").toBool();
        const QString statusText = response.value("status").toString();
        cmdStateLbl->setText(ok ? "acked" : (statusText.isEmpty() ? "failed" : statusText));
        if (response.contains("command_id")) {
            cmdIdLbl->setText(QString::number(response.value("command_id").toInt()));
        } else {
            cmdIdLbl->setText("-");
        }
        if (response.contains("latency_ms")) {
            cmdLatencyLbl->setText(QString::number(response.value("latency_ms").toInt()));
        } else {
            cmdLatencyLbl->setText("-");
        }
        const QString respError = response.value("error").toString();
        if (ok && error.isEmpty()) {
            cmdErrorLbl->setText(respError.isEmpty() ? "-" : respError);
        } else {
            cmdErrorLbl->setText(!respError.isEmpty() ? respError : (!error.isEmpty() ? error : QString("http %1").arg(httpStatus)));
        }
        api.refreshNow();
    });

    api.startPolling(1000);
    win.show();
    return app.exec();
}
