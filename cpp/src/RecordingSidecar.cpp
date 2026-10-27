#include "RecordingSidecar.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

#include <ic4/ic4.h>

#include "Camera.h"
#include "version.h"

namespace sidecar {

namespace {

// Each key setting read with its own type, so the JSON holds numbers and booleans, not strings.
// Missing properties (other camera models) are simply left out.
QJsonObject summary(ic4::PropertyMap props)
{
    QJsonObject out;
    auto real = [&](const char* key, const char* name) {
        ic4::Error e;
        const double v = props.getValueDouble(name, e);
        if (e.isSuccess())
            out[key] = v;
    };
    auto integer = [&](const char* key, const char* name) {
        ic4::Error e;
        const auto v = props.getValueInt64(name, e);
        if (e.isSuccess())
            out[key] = qint64(v);
    };
    auto text = [&](const char* key, const char* name) {
        ic4::Error e;
        const auto v = props.getValueString(name, e);
        if (e.isSuccess())
            out[key] = QString::fromStdString(v);
    };
    auto boolean = [&](const char* key, const char* name) {
        ic4::Error e;
        const bool v = props.getValueBool(name, e);
        if (e.isSuccess())
            out[key] = v;
    };

    integer("width", "Width");
    integer("height", "Height");
    text("pixel_format", "PixelFormat");
    real("frame_rate_setting_fps", "AcquisitionFrameRate");
    text("exposure_auto", "ExposureAuto");
    real("exposure_time_us", "ExposureTime");
    real("exposure_auto_upper_limit_us", "ExposureAutoUpperLimit");
    integer("exposure_auto_reference", "ExposureAutoReference");
    text("gain_auto", "GainAuto");
    real("gain_db", "Gain");
    real("gain_auto_upper_limit_db", "GainAutoUpperLimit");
    text("white_balance_auto", "BalanceWhiteAuto");
    QJsonObject ratios;
    for (const char* channel : {"Red", "Green", "Blue"}) {
        ic4::Error e;
        props.setValue("BalanceRatioSelector", channel, e);
        const double v = props.getValueDouble("BalanceRatio", e);
        if (e.isSuccess())
            ratios[QString(channel).toLower()] = v;
    }
    if (!ratios.isEmpty())
        out["white_balance_ratios"] = ratios;
    real("gamma", "Gamma");
    real("black_level", "BlackLevel");
    boolean("flip_horizontal", "ReverseX");
    boolean("flip_vertical", "ReverseY");
    real("sensor_temperature_c", "DeviceTemperature");
    return out;
}

}  // namespace

QJsonValue cameraSnapshot(Camera& camera)
{
    const QByteArray state = camera.stateJson();
    if (state.isEmpty())
        return QJsonValue::Null;
    const QJsonObject full = QJsonDocument::fromJson(state).object();
    QJsonObject out;
    out["captured_at"] = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    out["device"] = full.value("device");
    out["summary"] = summary(camera.properties());
    out["ic4_state"] = full;
    return out;
}

QString write(const QString& path, const QJsonObject& object)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return file.errorString();
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    return file.commit() ? QString() : file.errorString();
}

QString appVersion()
{
    return QStringLiteral(SCOPE_RECORDER_VERSION);
}

}  // namespace sidecar
