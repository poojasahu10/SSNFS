#include "ssnfsworker.h"
#include <log.h>
#include <serversettings.h>

#include <PH7/ph7.h>
#include <QMimeDatabase>
#include <QFileInfo>

static int PH7Consumer(const void *pOutput, unsigned int nOutputLen, void *pUserData) {
    ((SSNFSWorker*)pUserData)->httpResponse.append((const char*)pOutput, nOutputLen);
    return PH7_OK;
}
static int PH7SetHeader(ph7_context *pCtx,int argc,ph7_value **argv) {
    SSNFSWorker *worker = (SSNFSWorker*)ph7_context_user_data(pCtx);
    if (argc >= 1 && ph7_value_is_string(argv[0])) {
        int headerLen;
        const char *headerStr = ph7_value_to_string(argv[0], &headerLen);
        QVector<QString> headerParts;
        QString header(QByteArray(headerStr, headerLen));
        int HeaderNameLen = header.indexOf(": ");
        if (HeaderNameLen == -1) {
            ph7_context_throw_error(pCtx, PH7_CTX_WARNING, "The specified header does not contain \": \". Header key/value pairs must be separated by a \": \".");
            ph7_result_bool(pCtx, 0);
            return PH7_OK;
        }
        bool replace = true;
        if (argc >= 2 && ph7_value_is_bool(argv[1]) && ph7_value_to_bool(argv[1]) == 0)
            replace = false;

        if (replace) {
            worker->responseHeaders.insert(header.mid(0, HeaderNameLen), header.mid(HeaderNameLen + 2));
        } else {
            worker->responseHeaders.insertMulti(header.mid(0, HeaderNameLen), header.mid(HeaderNameLen + 2));
        }
    }

    ph7_result_bool(pCtx, 1);
    return PH7_OK;
}
static int PH7ResponseCode(ph7_context *pCtx,int argc,ph7_value **argv) {
    SSNFSWorker *worker = (SSNFSWorker*)ph7_context_user_data(pCtx);
    ph7_result_int(pCtx, worker->httpResultCode);
    if (argc >= 1 && ph7_value_is_int(argv[0])) {
        int newCode = ph7_value_to_int(argv[0]);
        if (worker->knownResultCodes.keys().contains(newCode)) {
            worker->httpResultCode = newCode;
        } else {
            ph7_context_throw_error(pCtx, PH7_CTX_WARNING, "Invalid or unsupported HTTP return code specified.");
        }
    }
    return PH7_OK;
}
static int PH7CheckAuthCookie(ph7_context *pCtx,int argc,ph7_value **argv) {
    SSNFSWorker *worker = (SSNFSWorker*)ph7_context_user_data(pCtx);

    if (argc == 1 && ph7_value_is_string(argv[0])) {
        QString cookie(ph7_value_to_string(argv[0], 0));

        QSqlQuery getUserKey(worker->configDB);
        getUserKey.prepare(R"(
            SELECT `User_Key`
            FROM `Web_Tokens`
            WHERE `Token` = ? AND ((julianday('now') - julianday(LastAccess_TmStmp)) * 24 * 60) > 30;)");
        getUserKey.addBindValue(cookie);
        if (getUserKey.exec()) {
            if (getUserKey.next()) {
                worker->webUserKey = getUserKey.value(0).toLongLong();

                ph7_result_bool(pCtx, true);
            } else {
                ph7_result_bool(pCtx, false);
            }
        } else {
            Log::error(Log::Categories["Web Server"], "Error while checking auth cookie: {0}", ToChr(getUserKey.lastError().text()));
            ph7_context_throw_error(pCtx, PH7_CTX_ERR, "Internal error while verifying the auth cookie.");
        }
    } else {
        ph7_context_throw_error(pCtx, PH7_CTX_ERR, "An invalid number or type of argument(s) was specified.");
    }

    return PH7_OK;
}

// TODO: Read this from file?
#define HTTP_500_RESPONSE "HTTP/1.1 500 Internal Server Error\r\n" \
                          "Connection: close\r\n\r\n" \
                          "<html><body>\n" \
                          "<h3>An error occured while processing your request.</h3>\n" \
                          "This error has been logged and will be investigated shortly.\n" \
                          "</body></html>"
void SSNFSWorker::processHttpRequest(char firstChar)
{
    // Before we even do anything, delete Web Tokens which have not been accessed in the last 30 min.
    QSqlQuery delOldTokens(configDB);
    delOldTokens.prepare("DELETE FROM `Web_Tokens` WHERE ((julianday('now') - julianday(LastAccess_TmStmp)) * 24 * 60) > 30;");
    if (delOldTokens.exec() == false) {
        Log::error(Log::Categories["Web Server"], "Error removing old web authentication tokens: {0}", ToChr(delOldTokens.lastError().text()));
    }

    QByteArray Request;
    Request.append(firstChar);

    while (socket->canReadLine() == false) {
        socket->waitForReadyRead(-1);
    }
    QString RequestLine = socket->readLine();
    Request.append(RequestLine);
    QString RequestPath = RequestLine.split(" ")[1];

    QString FinalPath = ServerSettings::get("WebPanelPath");
    FinalPath.append(Common::resolveRelative(RequestPath));

    QFileInfo FileFI(FinalPath);
    if (!FileFI.exists()) {
        socket->write("HTTP/1.1 404 Not Found\r\n");
        socket->write("Content-Type: text/html\r\n");
        socket->write("Connection: close\r\n\r\n");
        socket->write("<html><body><h3>The file or directory you requested does not exist.</h3></body></html>");
        return;
    }
    if (FileFI.isDir()) {
        if (!FinalPath.endsWith('/'))
            FinalPath.append('/');
        FinalPath.append("index.php");
    }
    FileFI.setFile(FinalPath);
    if (!FileFI.exists()) {
        socket->write("HTTP/1.1 404 Not Found\r\n");
        socket->write("Content-Type: text/html\r\n");
        socket->write("Connection: close\r\n\r\n");
        socket->write("<html><body><h3>The file or directory you requested does not exist.</h3></body></html>");
        return;
    }

    if (!FileFI.isReadable()) {
        socket->write("HTTP/1.1 403 Forbidden\r\n");
        socket->write("Content-Type: text/html\r\n");
        socket->write("Connection: close\r\n\r\n");
        socket->write("<html><body><h3>The file or directory you requested cannot be opened.</h3></body></html>");
        return;
    }

    QMap<QString, QString> Headers;
    while (true) {
        if (!socket->isOpen() || !socket->isEncrypted()) {
            return;
        }
        while (socket->canReadLine() == false) {
            if (!socket->waitForReadyRead(3000))
                continue;
        }
        QString HeaderLine = socket->readLine();
        Request.append(HeaderLine);
        HeaderLine = HeaderLine.trimmed();
        if (HeaderLine.isEmpty()) {
            break;
        }
        int HeaderNameLen = HeaderLine.indexOf(": ");
        Headers.insert(HeaderLine.mid(0, HeaderNameLen), HeaderLine.mid(HeaderNameLen + 2));
    }

    if (Headers.keys().contains("Content-Length")) {
        bool lengthOK;
        uint reqLength = Headers["Content-Length"].toUInt(&lengthOK);
        if (lengthOK) {
            while (reqLength > 0) {
                if (socket->bytesAvailable() == 0) {
                    if (!socket->waitForReadyRead(3000))
                        continue;
                }
                QByteArray currBatch = socket->readAll();
                reqLength -= currBatch.length();
                Request.append(currBatch);
            }
        }
    }

    if (FinalPath.endsWith(".php", Qt::CaseInsensitive)) {
        ph7 *pEngine; /* PH7 engine */
        ph7_vm *pVm; /* Compiled PHP program */
        int rc;
        /* Allocate a new PH7 engine instance */
        rc = ph7_init(&pEngine);
        if( rc != PH7_OK ){
            /*
            * If the supplied memory subsystem is so sick that we are unable
            * to allocate a tiny chunk of memory, there is not much we can do here.
            */
            Log::error(Log::Categories["Web Server"], "Error while allocating a new PH7 engine instance");
            socket->write(HTTP_500_RESPONSE);
            return;
        }
        /* Compile the PHP test program defined above */
        rc = ph7_compile_file(
                    pEngine,
                    ToChr(FinalPath),
                    &pVm,
                    0);
        if( rc != PH7_OK ){
            if( rc == PH7_COMPILE_ERR ){
                const char *zErrLog;
                int nLen;
                /* Extract error log */
                ph7_config(pEngine,
                           PH7_CONFIG_ERR_LOG,
                           &zErrLog,
                           &nLen
                           );
                if( nLen > 0 ){
                    /* zErrLog is null terminated */
                    Log::error(Log::Categories["Web Server"], zErrLog);
                    socket->write(HTTP_500_RESPONSE);
                    return;
                }
            }
            Log::error(Log::Categories["Web Server"], "PH7: Unknown compile error.");
            socket->write(HTTP_500_RESPONSE);
            return;
        }
        /*
         * Now we have our script compiled, it's time to configure our VM.
         * We will install the output consumer callback defined above
         * so that we can consume and redirect the VM output to STDOUT.
         */
        rc = ph7_vm_config(pVm,
            PH7_VM_CONFIG_OUTPUT,
            PH7Consumer,
            this /* Callback private data */
            );
        if( rc != PH7_OK ){
            socket->write(HTTP_500_RESPONSE);
            Log::error(Log::Categories["Web Server"], "Error while installing the VM output consumer callback");
            return;
        }
        rc = ph7_vm_config(pVm,
            PH7_VM_CONFIG_HTTP_REQUEST,
            Request.data(),
            Request.length()
            );
        if( rc != PH7_OK ) {
            socket->write(HTTP_500_RESPONSE);
            Log::error(Log::Categories["Web Server"], "Error while transferring the HTTP request to PH7.");
            return;
        }

        rc = ph7_create_function(
                    pVm,
                    "header",
                    PH7SetHeader,
                    this);
        if( rc != PH7_OK ) {
            socket->write(HTTP_500_RESPONSE);
            Log::error(Log::Categories["Web Server"], "Error while setting up header() function in PH7.");
            return;
        }
        rc = ph7_create_function(
                    pVm,
                    "http_response_code",
                    PH7ResponseCode,
                    this);
        if( rc != PH7_OK ) {
            socket->write(HTTP_500_RESPONSE);
            Log::error(Log::Categories["Web Server"], "Error while setting up http_response_code() function in PH7.");
            return;
        }
        rc = ph7_create_function(
                    pVm,
                    "check_auth_cookie",
                    PH7CheckAuthCookie,
                    this);
        if( rc != PH7_OK ) {
            socket->write(HTTP_500_RESPONSE);
            Log::error(Log::Categories["Web Server"], "Error while setting up check_auth_cookie() function in PH7.");
            return;
        }

        /*
        * And finally, execute our program.
        */
        if (ph7_vm_exec(pVm,0) != PH7_OK) {
            socket->write(HTTP_500_RESPONSE);
            Log::error(Log::Categories["Web Server"], "Error occurred while running PH7 script for script '{0}'.");
            return;
        }
        /* All done, cleanup the mess left behind.
        */
        ph7_vm_release(pVm);
        ph7_release(pEngine);

        // Force Connection header to close since implementing keep-alive is not planned.
        responseHeaders["Connection"] = "close";
        socket->write(tr("HTTP/1.1 %1 %2\r\n").arg(httpResultCode).arg(knownResultCodes.value(httpResultCode)).toUtf8());
        for (QHash<QString, QString>::iterator i = responseHeaders.begin(); i != responseHeaders.end(); ++i) {
            socket->write(i.key().toUtf8());
            socket->write(": ");
            socket->write(i.value().toUtf8());
            socket->write("\r\n");
        }
        socket->write("\r\n");
        socket->write(httpResponse);
    } else {
        socket->write("HTTP/1.1 200 OK\r\n");
        QMimeDatabase mimeDB;
        socket->write(ToChr(tr("Content-Type: %1\r\n").arg(mimeDB.mimeTypeForFile(FinalPath).name())));
        // Let the browser know we plan to close this conenction.
        socket->write("Connection: close\r\n\r\n");
        QFile requestedFile(FinalPath);
        requestedFile.open(QFile::ReadOnly);
        while (!requestedFile.atEnd()) {
            socket->write(requestedFile.readAll());
        }
        requestedFile.close();
        socket->waitForBytesWritten(-1);
    }
}