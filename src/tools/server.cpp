#include <App.h>
#include <string>
#include <mutex>
#include <thread>
#include <string_view>
#include <atomic>
#include <experimental/filesystem>
#include "../core/logger.hpp"
#include "../core/crypto.hpp"
#include "../core/host_manager.hpp"
#include "../core/helpers.hpp"
#include "../core/request_manager.hpp"
#include "../core/api.hpp"
#include "../core/crypto.hpp"
using namespace std;

int main(int argc, char **argv) {    
    json config = readJsonFromFile(DEFAULT_CONFIG_FILE_PATH);

    int port = config["port"];

    if (argc > 1) {
        string logfile = string(argv[1]);
        Logger::file.open(logfile);
    }
    HostManager hosts(config);
    RequestManager manager(hosts);
    manager.setTaxRate(TAX_RATE);
 
    uWS::App().get("/block_count", [&manager](auto *res, auto *req) {
        try {
            std::string count = manager.getBlockCount();
            res->writeHeader("Content-Type", "text/html; charset=utf-8")->end(count);
        } catch(const std::exception &e) {
            Logger::logError("/block_count", e.what());
        } catch(...) {
            Logger::logError("/block_count", "unknown");
        }
    }).get("/stats", [&manager](auto *res, auto *req) {
        try {
            json stats = manager.getStats();
            res->writeHeader("Content-Type", "application/json; charset=utf-8")->end(stats.dump());
        } catch(const std::exception &e) {
            Logger::logError("/stats", e.what());
        } catch(...) {
            Logger::logError("/stats", "unknown");
        }
    }).get("/block/:b", [&manager](auto *res, auto *req) {
        json result;
        try {
            int idx = std::stoi(string(req->getParameter(0)));
            int count = std::stoi(manager.getBlockCount());
            if (idx < 0 || idx > count) {
                result["error"] = "Invalid Block";
            } else {
                result = manager.getBlock(idx);
            }
            res->writeHeader("Content-Type", "application/json; charset=utf-8")->end(result.dump());
        } catch(const std::exception &e) {
            result["error"] = "Unknown error";
            Logger::logError("/block", e.what());
        } catch(...) {
            Logger::logError("/block", "unknown");
        }
    }).get("/ledger/:user", [&manager](auto *res, auto *req) {
        try {
            PublicWalletAddress w = stringToWalletAddress(string(req->getParameter(0)));
            json ledger = manager.getLedger(w);
            res->writeHeader("Content-Type", "application/json; charset=utf-8")->end(ledger.dump());
        } catch(const std::exception &e) {
            Logger::logError("/ledger", e.what());
        } catch(...) {
            Logger::logError("/ledger", "unknown");
        }
    }).post("/submit", [&manager](auto *res, auto *req) {
        res->onAborted([res]() {
            res->end("ABORTED");
        });
        std::string buffer;
        res->onData([res, buffer = std::move(buffer), &manager](std::string_view data, bool last) mutable {
            buffer.append(data.data(), data.length());
            if (last) {
                try {
                    if (buffer.length() < sizeof(BlockHeader) + sizeof(TransactionInfo)) {
                        json response;
                        response["error"] = "Malformed block";
                        res->end(response.dump());
                        Logger::logError("/submit","Malformed block");
                        return;
                    }
                    char * ptr = (char*)buffer.c_str();
                    BlockHeader blockH = *((BlockHeader*)ptr);
                    ptr += sizeof(BlockHeader);
                    vector<Transaction> transactions;
                    if (blockH.numTransactions > MAX_TRANSACTIONS_PER_BLOCK) {
                        json response;
                        response["error"] = "Too many transactions";
                        res->end(response.dump());
                        Logger::logError("/submit","Too many transactions");
                    } else {
                        for(int j = 0; j < blockH.numTransactions; j++) {
                            TransactionInfo t = *((TransactionInfo*)ptr);
                            ptr += sizeof(TransactionInfo);
                            transactions.push_back(Transaction(t));
                        }
                        Block block(blockH, transactions);
                        json response = manager.submitProofOfWork(block);
                        res->end(response.dump());
                    }
                } catch(const std::exception &e) {
                    json response;
                    response["error"] = string(e.what());
                    res->end(response.dump());
                    Logger::logError("/submit", e.what());
                } catch(...) {
                    json response;
                    response["error"] = "unknown";
                    res->end(response.dump());
                    Logger::logError("/submit", "unknown");
                }
                
            }
        });
    }).get("/mine", [&manager](auto *res, auto *req) {
        try {
            json response = manager.getProofOfWork();
            res->end(response.dump());
        } catch(const std::exception &e) {
            Logger::logError("/mine", e.what());
        } catch(...) {
            Logger::logError("/mine", "unknown");
        }
    }).get("/sync/:start/:end", [&manager](auto *res, auto *req) {
        try {
            int start = std::stoi(string(req->getParameter(0)));
            int end = std::stoi(string(req->getParameter(1)));
            if ((end-start) > BLOCKS_PER_FETCH) {
                Logger::logError("/sync", "invalid range requested");
                res->end("");
            }
            res->writeHeader("Content-Type", "application/octet-stream");
            for (int i = start; i <=end; i++) {
                std::pair<char*, size_t> buffer = manager.getRawBlockData(i);
                std::string_view str(buffer.first, buffer.second);
                res->write(str);
                delete buffer.first;
            }
            res->end("");
        } catch(const std::exception &e) {
            Logger::logError("/sync", e.what());
        } catch(...) {
            Logger::logError("/sync", "unknown");
        }
        res->onAborted([res]() {
            res->end("ABORTED");
        });
    }).get("/synctx", [&manager](auto *res, auto *req) {
        try {
            res->writeHeader("Content-Type", "application/octet-stream");
            std::pair<char*, size_t> buffer = manager.getRawTransactionData();
            std::string_view str(buffer.first, buffer.second);
            res->write(str);
            delete buffer.first;
            res->end("");
        } catch(const std::exception &e) {
            Logger::logError("/sync", e.what());
        } catch(...) {
            Logger::logError("/sync", "unknown");
        }
        res->onAborted([res]() {
            res->end("ABORTED");
        });
    }).post("/add_transaction", [&manager](auto *res, auto *req) {
        res->onAborted([res]() {
            res->end("ABORTED");
        });
        std::string buffer;
        res->onData([res, buffer = std::move(buffer), &manager](std::string_view data, bool last) mutable {
            buffer.append(data.data(), data.length());
            if (last) {
                try {
                    if (buffer.length() < sizeof(TransactionInfo)) {
                        json response;
                        response["error"] = "Malformed transaction";
                        res->end(response.dump());
                        Logger::logError("/add_transaction","Malformed transaction");
                        return;
                    }
                    TransactionInfo t = *((TransactionInfo*)buffer.c_str());
                    Transaction tx(t);
                    json response = manager.addTransaction(tx);
                    res->end(response.dump());
                }  catch(const std::exception &e) {
                    Logger::logError("/add_transaction", e.what());
                } catch(...) {
                    Logger::logError("/add_transaction", "unknown");
                }
            }
        });
    }).post("/verify_transaction", [&manager](auto *res, auto *req) {
        /* Allocate automatic, stack, variable as usual */
        std::string buffer;
        /* Move it to storage of lambda */
        res->onData([res, buffer = std::move(buffer), &manager](std::string_view data, bool last) mutable {
            buffer.append(data.data(), data.length());
            if (last) {
                if (buffer.length() < sizeof(TransactionInfo)) {
                    json response;
                    response["error"] = "Malformed transaction";
                    res->end(response.dump());
                    Logger::logError("/verify_transaction","Malformed transaction");
                    return;
                }
                TransactionInfo t = *((TransactionInfo*)buffer.c_str());
                Transaction tx(t);
                json response = manager.verifyTransaction(tx);
                res->end(response.dump());
            }
        });
        res->onAborted([res]() {
            res->end("ABORTED");
        });
    }).listen(port, [port](auto *token) {
        Logger::logStatus("Started server");
    }).run();

}

