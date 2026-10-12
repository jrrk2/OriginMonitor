#pragma once

#include <QJsonObject>
#include <QString>

/**
 * Sends the observing briefings by Microsoft 365 / Outlook.com mail, with the OAuth 2.0 device-code flow (Microsoft's sign-in for clients with no browser)
 * and Microsoft Graph over HTTPS. No password is stored and SMTP AUTH is not used (Microsoft is retiring basic-auth SMTP submission).
 *
 * One-time setup: register an app in Microsoft Entra (public client, "Allow public client flows" = yes, delegated permission Mail.Send, no secret), then
 *   ~/.config/origin/mail.conf  {"client_id": "...", "tenant": "common|organizations|<tenant id>", "to": "jonathan@kimmitt.co.uk"}   (mode 600)
 *   OriginHeadless mail-login   -> prints a code and a URL; sign in once as the mailbox that should send; the refresh token is kept in
 *   ~/.config/origin/mail_token.json (mode 600) and renewed on every send (a token unused for 90 days expires: log in again).
 * The message is sent from the signed-in mailbox (Graph /me/sendMail) to "to".
 */
namespace Mail {

QString configPath();
QJsonObject login();                                                      // device-code sign-in; blocks until done or expired; {ok, ...}
QJsonObject send(const QString& subject, const QString& body, const QString& to = QString());   // {ok} or {ok:false, error}

}   // namespace Mail
