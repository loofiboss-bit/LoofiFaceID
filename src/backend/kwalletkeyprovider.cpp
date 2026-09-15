// SPDX-License-Identifier: GPL-3.0-or-later

#include "kwalletkeyprovider.h"

#include <KWallet>

#include <openssl/rand.h>

namespace
{
const QString Folder = QStringLiteral("KFaceAuth");
const QString Entry = QStringLiteral("user-session-vault-master-key-v1");
constexpr qsizetype KeyBytes = 32;
} // namespace

KWalletKeyProvider::KWalletKeyProvider(QObject *parent) : QObject(parent)
{
    m_accessTimer.setSingleShot(true);
    m_accessTimer.setInterval(15000);
    connect(&m_accessTimer, &QTimer::timeout, this,
            [this]()
            {
                if (m_completion)
                    finish(Result{State::Unavailable, {}});
            });
}

KWalletKeyProvider::~KWalletKeyProvider()
{
    cancel();
    delete m_wallet;
}

KWalletKeyProvider::State KWalletKeyProvider::boundedState() const
{
    if (!KWallet::Wallet::isEnabled())
        return State::Unavailable;
    return KWallet::Wallet::isOpen(KWallet::Wallet::NetworkWallet()) ? State::Available : State::Locked;
}

void KWalletKeyProvider::requestKey(Completion completion)
{
    if (m_completion)
    {
        if (completion)
            completion(Result{State::Unavailable, {}});
        return;
    }
    if (!KWallet::Wallet::isEnabled())
    {
        if (completion)
            completion(Result{State::Unavailable, {}});
        return;
    }
    m_completion = std::move(completion);
    m_action = PendingAction::Read;
    if (m_wallet && m_wallet->isOpen())
    {
        performRead();
        return;
    }
    delete m_wallet;
    m_wallet = KWallet::Wallet::openWallet(KWallet::Wallet::NetworkWallet(), 0, KWallet::Wallet::Asynchronous);
    if (!m_wallet)
    {
        finish(Result{State::Unavailable, {}});
        return;
    }
    connect(m_wallet, &KWallet::Wallet::walletOpened, this, &KWalletKeyProvider::finishOpen, Qt::SingleShotConnection);
    m_accessTimer.start();
}

KWalletKeyProvider::Result KWalletKeyProvider::generateTransientKey() const
{
    QByteArray key(KeyBytes, 0);
    if (RAND_priv_bytes(reinterpret_cast<unsigned char *>(key.data()), static_cast<int>(key.size())) != 1)
    {
        key.fill(0);
        return Result{State::Unavailable, {}};
    }
    return Result{State::Available, std::move(key)};
}

void KWalletKeyProvider::storeKey(QByteArray key, Completion completion)
{
    if (m_completion || key.size() != KeyBytes || !KWallet::Wallet::isEnabled())
    {
        key.fill(0);
        if (completion)
            completion(Result{State::Unavailable, {}});
        return;
    }
    m_completion = std::move(completion);
    m_action = PendingAction::Store;
    m_pendingKey = std::move(key);
    if (m_wallet && m_wallet->isOpen())
    {
        performRead();
        return;
    }
    delete m_wallet;
    m_wallet = KWallet::Wallet::openWallet(KWallet::Wallet::NetworkWallet(), 0, KWallet::Wallet::Asynchronous);
    if (!m_wallet)
    {
        finish(Result{State::Unavailable, {}});
        return;
    }
    connect(m_wallet, &KWallet::Wallet::walletOpened, this, &KWalletKeyProvider::finishOpen, Qt::SingleShotConnection);
    m_accessTimer.start();
}

void KWalletKeyProvider::deleteKey(Completion completion)
{
    if (m_completion)
    {
        if (completion)
            completion(Result{State::Unavailable, {}});
        return;
    }
    m_completion = std::move(completion);
    m_action = PendingAction::Delete;
    if (!KWallet::Wallet::isEnabled())
    {
        finish(Result{State::Unavailable, {}});
        return;
    }
    if (m_wallet && m_wallet->isOpen())
    {
        performRead();
        return;
    }
    delete m_wallet;
    m_wallet = KWallet::Wallet::openWallet(KWallet::Wallet::NetworkWallet(), 0, KWallet::Wallet::Asynchronous);
    if (!m_wallet)
    {
        finish(Result{State::Unavailable, {}});
        return;
    }
    connect(m_wallet, &KWallet::Wallet::walletOpened, this, &KWalletKeyProvider::finishOpen, Qt::SingleShotConnection);
    m_accessTimer.start();
}

void KWalletKeyProvider::cancel()
{
    if (m_completion)
        finish(Result{State::Cancelled, {}});
}

void KWalletKeyProvider::finishOpen(bool success)
{
    if (!m_completion || m_action == PendingAction::None)
        return;
    if (!success)
    {
        finish(Result{State::Cancelled, {}});
        return;
    }
    performRead();
}

void KWalletKeyProvider::performRead()
{
    if (!m_wallet || !m_wallet->isOpen())
    {
        finish(Result{State::Unavailable, {}});
        return;
    }
    const bool store = m_action == PendingAction::Store;
    if (!m_wallet->hasFolder(Folder) && !store)
    {
        finish(Result{State::Absent, {}});
        return;
    }
    if (!prepareFolder(store))
    {
        finish(Result{State::Unavailable, {}});
        return;
    }
    if (m_action == PendingAction::Delete)
    {
        if (m_wallet->hasEntry(Entry) && m_wallet->removeEntry(Entry) != 0)
        {
            finish(Result{State::Unavailable, {}});
            return;
        }
        finish(Result{State::Absent, {}});
        return;
    }

    if (m_action == PendingAction::Store)
    {
        if (m_wallet->hasEntry(Entry) || m_wallet->writeEntry(Entry, m_pendingKey, KWallet::Wallet::Stream) != 0)
        {
            finish(Result{State::Unavailable, {}});
            return;
        }
        finish(Result{State::Available, {}});
        return;
    }

    if (m_action != PendingAction::Read)
    {
        finish(Result{State::Unavailable, {}});
        return;
    }

    QByteArray key;
    if (m_wallet->hasEntry(Entry))
    {
        if (m_wallet->entryType(Entry) != KWallet::Wallet::Stream || m_wallet->readEntry(Entry, key) != 0 ||
            key.size() != KeyBytes)
        {
            key.fill(0);
            finish(Result{State::Unavailable, {}});
            return;
        }
        finish(Result{State::Available, std::move(key)});
        return;
    }
    finish(Result{State::Absent, {}});
}

void KWalletKeyProvider::finish(Result result)
{
    m_accessTimer.stop();
    if (m_wallet)
        disconnect(m_wallet, &KWallet::Wallet::walletOpened, this, &KWalletKeyProvider::finishOpen);
    const auto completion = std::move(m_completion);
    m_completion = {};
    m_action = PendingAction::None;
    m_pendingKey.fill(0);
    m_pendingKey.clear();
    if (completion)
        completion(std::move(result));
    else
        result.clear();
}

bool KWalletKeyProvider::prepareFolder(bool create)
{
    if (!m_wallet)
        return false;
    if (!m_wallet->hasFolder(Folder))
    {
        if (!create)
            return true;
        if (!m_wallet->createFolder(Folder))
            return false;
    }
    return m_wallet->hasFolder(Folder) && m_wallet->setFolder(Folder);
}
