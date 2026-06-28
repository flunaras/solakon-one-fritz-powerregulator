#include "secretstore.h"

#include <keychain.h>

namespace {
// Service name under which all keys are grouped in the platform secret
// store (shows up as the "application"/"service" field in e.g. Seahorse or
// the KWallet manager).
constexpr char kService[] = "solakon-one-fritz-powerregulator";
}

QString SecretStore::keyFor(const QString& host, int port) {
    return host + ":" + QString::number(port);
}

SecretStore::SecretStore(QObject* parent) : QObject(parent) {
}

void SecretStore::load(const QString& host, int port) {
    auto* job = new QKeychain::ReadPasswordJob(kService, this);
    job->setKey(keyFor(host, port));
    connect(job, &QKeychain::Job::finished, this, [this, job]() {
        // EntryNotFound just means no key has ever been saved for this
        // connection -- not an error worth surfacing to the user.
        if (job->error() != QKeychain::NoError
            && job->error() != QKeychain::EntryNotFound) {
            emit apiKeyError(job->errorString());
            return;
        }
        emit apiKeyLoaded(job->error() == QKeychain::NoError ? job->textData() : QString());
    });
    job->start();
}

void SecretStore::save(const QString& host, int port, const QString& apiKey) {
    auto* job = new QKeychain::WritePasswordJob(kService, this);
    job->setKey(keyFor(host, port));
    job->setTextData(apiKey);
    connect(job, &QKeychain::Job::finished, this, [this, job]() {
        if (job->error() != QKeychain::NoError) {
            emit apiKeyError(job->errorString());
            return;
        }
        emit apiKeySaved();
    });
    job->start();
}

void SecretStore::remove(const QString& host, int port) {
    auto* job = new QKeychain::DeletePasswordJob(kService, this);
    job->setKey(keyFor(host, port));
    connect(job, &QKeychain::Job::finished, this, [this, job]() {
        if (job->error() != QKeychain::NoError
            && job->error() != QKeychain::EntryNotFound) {
            emit apiKeyError(job->errorString());
        }
    });
    job->start();
}
