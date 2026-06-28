#include "connectiondialog.h"
#include "secretstore.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QVBoxLayout>

ConnectionDialog::ConnectionDialog(SecretStore* secretStore, QWidget* parent)
    : QDialog(parent), m_secretStore(secretStore) {
    setWindowTitle(tr("Connect to solakon-one-fritz-powerregulator"));

    m_hostEdit = new QLineEdit(this);
    m_portSpin = new QSpinBox(this);
    m_portSpin->setRange(1, 65535);
    m_schemeCombo = new QComboBox(this);
    m_schemeCombo->addItems({"http", "https"});
    m_ignoreSslCheck = new QCheckBox(tr("Ignore TLS certificate errors "
                                        "(needed for the tool's self-signed certificate)"), this);
    m_intervalSpin = new QSpinBox(this);
    m_intervalSpin->setRange(2, 3600);
    m_intervalSpin->setSuffix(tr(" s"));

    m_apiKeyEdit = new QLineEdit(this);
    m_apiKeyEdit->setEchoMode(QLineEdit::Password);
    m_apiKeyEdit->setPlaceholderText(tr("stored in the system secret store"));

    m_autoConnectCheck = new QCheckBox(tr("Automatically connect to this server at startup"), this);

    auto* form = new QFormLayout;
    form->addRow(tr("Host"), m_hostEdit);
    form->addRow(tr("Port"), m_portSpin);
    form->addRow(tr("Scheme"), m_schemeCombo);
    form->addRow(QString(), m_ignoreSslCheck);
    form->addRow(tr("Poll interval"), m_intervalSpin);
    form->addRow(tr("API key"), m_apiKeyEdit);
    form->addRow(QString(), new QLabel(
        tr("The API key is stored only in your desktop's secret store "
           "(Secret Service/libsecret, KWallet, etc.) -- never in a config file."),
        this));
    form->addRow(QString(), m_autoConnectCheck);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_buttons);

    connect(m_hostEdit, &QLineEdit::editingFinished, this, &ConnectionDialog::onHostOrPortChanged);
    connect(m_portSpin, &QSpinBox::editingFinished, this, &ConnectionDialog::onHostOrPortChanged);
}

void ConnectionDialog::setSettings(const ConnectionSettings& s) {
    m_hostEdit->setText(s.host);
    m_portSpin->setValue(s.port);
    m_schemeCombo->setCurrentText(s.scheme);
    m_ignoreSslCheck->setChecked(s.ignoreSslErrors);
    m_intervalSpin->setValue(s.pollIntervalS);
    m_autoConnectCheck->setChecked(s.autoConnectOnStartup);
}

ConnectionSettings ConnectionDialog::settings() const {
    ConnectionSettings s;
    s.host = m_hostEdit->text().trimmed();
    s.port = m_portSpin->value();
    s.scheme = m_schemeCombo->currentText();
    s.ignoreSslErrors = m_ignoreSslCheck->isChecked();
    s.pollIntervalS = m_intervalSpin->value();
    s.autoConnectOnStartup = m_autoConnectCheck->isChecked();
    return s;
}

void ConnectionDialog::setApiKeyField(const QString& apiKey) {
    m_lastLoadedApiKey = apiKey;
    m_apiKeyEdit->setText(apiKey);
}

bool ConnectionDialog::apiKeyChanged() const {
    return m_apiKeyEdit->text() != m_lastLoadedApiKey;
}

QString ConnectionDialog::apiKey() const {
    return m_apiKeyEdit->text();
}

void ConnectionDialog::onHostOrPortChanged() {
    // Re-load whichever key belongs to the newly typed host/port, so the
    // user isn't shown (or, worse, about to overwrite) a stale connection's
    // key. Any local edit not yet saved is discarded -- reasonable given the
    // user just changed which server they're talking about.
    if (m_secretStore)
        m_secretStore->load(m_hostEdit->text().trimmed(), m_portSpin->value());
}
