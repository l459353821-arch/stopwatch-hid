import Foundation

/// DeepSeek `/user/balance` 响应体
struct DeepSeekBalance: Codable {
    let isAvailable: Bool
    let balanceInfos: [BalanceInfo]

    struct BalanceInfo: Codable {
        let currency: String
        let totalBalance: String
        let grantedBalance: String
        let toppedUpBalance: String
    }
}

enum DeepSeekBalanceError: LocalizedError {
    case http(Int, String)
    case noBalance

    var errorDescription: String? {
        switch self {
        case .http(let code, let body):
            return "HTTP \(code): \(body)"
        case .noBalance:
            return "余额接口未返回 balance_infos"
        }
    }
}

/// 抓取 DeepSeek 余额（同步阻塞，便于 CLI 使用）。API key 通过构造器传入。
struct DeepSeekBalanceClient {
    let apiKey: String
    let endpoint = URL(string: "https://api.deepseek.com/user/balance")!

    init(apiKey: String) {
        self.apiKey = apiKey
    }

    /// 返回 CNY 的 total_balance 字符串（如 "12.34"）
    func fetchBalance() throws -> String {
        var request = URLRequest(url: endpoint)
        request.httpMethod = "GET"
        request.setValue("Bearer \(apiKey)", forHTTPHeaderField: "Authorization")
        request.setValue("application/json", forHTTPHeaderField: "Accept")

        let semaphore = DispatchSemaphore(value: 0)
        var boxed: Result<String, Error>?

        URLSession.shared.dataTask(with: request) { data, response, error in
            defer { semaphore.signal() }
            if let error {
                boxed = .failure(error)
                return
            }
            if let http = response as? HTTPURLResponse, !(200..<300).contains(http.statusCode) {
                let body = data.flatMap { String(data: $0, encoding: .utf8) } ?? ""
                boxed = .failure(DeepSeekBalanceError.http(http.statusCode, body))
                return
            }
            guard let data else {
                boxed = .failure(DeepSeekBalanceError.noBalance)
                return
            }
            do {
                let decoder = JSONDecoder()
                decoder.keyDecodingStrategy = .convertFromSnakeCase
                let decoded = try decoder.decode(DeepSeekBalance.self, from: data)
                guard let info = decoded.balanceInfos.first(where: { $0.currency == "CNY" })
                        ?? decoded.balanceInfos.first else {
                    boxed = .failure(DeepSeekBalanceError.noBalance)
                    return
                }
                boxed = .success(info.totalBalance)
            } catch {
                boxed = .failure(error)
            }
        }.resume()

        semaphore.wait()
        switch boxed {
        case .success(let value): return value
        case .failure(let error): throw error
        case nil: throw DeepSeekBalanceError.noBalance
        }
    }
}
