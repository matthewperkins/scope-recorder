#pragma once

#include <QJsonObject>
#include <QString>

class Camera;

// Per-recording metadata file (`<recording>.json`) for reproducibility: recording and encoding
// parameters, plus the camera's settings captured at start and at stop. Rewritten atomically,
// so it is always a complete, valid file even if the app crashes mid-recording.
namespace sidecar {

// Model, serial, a readable summary of the key settings, and IC4's complete settings dump.
// Returns null if no camera is open (e.g. it was just unplugged).
QJsonValue cameraSnapshot(Camera& camera);

// Writes `object` to `path` atomically. Returns an error message, or empty on success.
QString write(const QString& path, const QJsonObject& object);

// "built from" identifier: git description of the source tree at build time.
QString appVersion();

}  // namespace sidecar
