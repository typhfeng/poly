## 核心交易系统

| 项目                  | 语言       | 说明                   |
| --------------------- | ---------- | ---------------------- |
| ctf-exchange          | Solidity   | CTF交易所核心智能合约  |
| py-clob-client        | Python     | Python版CLOB客户端     |
| clob-client           | TypeScript | TypeScript版CLOB客户端 |
| rs-clob-client        | Rust       | Rust版CLOB客户端       |
| real-time-data-client | TypeScript | 实时数据推送客户端     |

## AI/自动化交易

| 项目                | 语言   | 说明                                 |
| ------------------- | ------ | ------------------------------------ |
| agents              | Python | AI代理自主交易框架                   |
| poly-market-maker   | Python | CLOB做市商程序                       |
| market-maker-keeper | Python | 通用做市商keeper框架(fork自MakerDAO) |

## 订单签名工具

| 项目               | 语言       | 说明                    |
| ------------------ | ---------- | ----------------------- |
| python-order-utils | Python     | Python订单生成/签名工具 |
| clob-order-utils   | TypeScript | TypeScript订单工具      |
| go-order-utils     | Go         | Go订单工具              |

## 智能合约

| 项目                             | 语言       | 说明                      |
| -------------------------------- | ---------- | ------------------------- |
| conditional-tokens-contracts     | Solidity   | Gnosis条件代币核心合约    |
| conditional-tokens-market-makers | JavaScript | AMM做市商合约             |
| uma-ctf-adapter                  | Solidity   | 通过UMA预言机解析市场结果 |
| neg-risk-ctf-adapter             | Solidity   | 负风险CTF适配器           |
| uma-sports-oracle                | Solidity   | 体育赛事预言机            |
| PolyLend                         | Solidity   | 借贷协议                  |
| exchange-fee-module              | Solidity   | 动态费率模块              |
| proxy-factories                  | Solidity   | 代理工厂合约              |

## Builder/Relayer系统

| 项目                      | 语言       | 说明                     |
| ------------------------- | ---------- | ------------------------ |
| builder-relayer-client    | TypeScript | TypeScript Relayer客户端 |
| py-builder-relayer-client | Python     | Python Relayer客户端     |
| builder-signing-sdk       | TypeScript | Builder认证签名SDK       |
| builder-signing-server    | TypeScript | Builder签名服务端        |
| go-builder-signing-sdk    | Go         | Go版签名SDK              |
| py-builder-signing-sdk    | Python     | Python版签名SDK          |

## 数据索引(Subgraph)

| 项目                | 语言       | 说明                           |
| ------------------- | ---------- | ------------------------------ |
| polymarket-subgraph | TypeScript | 主索引(交易、流动性、市场数据) |
| resolution-subgraph | TypeScript | 市场结算索引                   |
| positions-subgraph  | TypeScript | 持仓索引                       |
| tvl-subgraph        | TypeScript | TVL索引                        |

## SDK/钱包集成示例

| 项目                         | 语言       | 说明                  |
| ---------------------------- | ---------- | --------------------- |
| polymarket-sdk               | TypeScript | 钱包交互SDK           |
| wagmi-safe-builder-example   | TypeScript | Wagmi + Safe集成示例  |
| privy-safe-builder-example   | TypeScript | Privy + Safe示例      |
| magic-proxy-builder-example  | TypeScript | Magic Link代理示例    |
| magic-safe-builder-example   | TypeScript | Magic Link + Safe示例 |
| turnkey-safe-builder-example | TypeScript | Turnkey + Safe示例    |
| safe-wallet-integration      | TypeScript | Safe钱包集成          |
| polymarket-us-python         | Python     | 美国版Python SDK      |
| polymarket-us-typescript     | TypeScript | 美国版TypeScript SDK  |

## 工具/脚本

| 项目                          | 语言       | 说明                    |
| ----------------------------- | ---------- | ----------------------- |
| examples                      | TypeScript | 官方代码示例集          |
| py-merge-split-positions      | Python     | Python版仓位合并/拆分   |
| ts-merge-split-positions      | TypeScript | TypeScript版仓位操作    |
| poly-ct-scripts               | Solidity   | Forge脚本(条件代币交互) |
| conditional-token-examples    | TypeScript | Ethers条件代币示例      |
| conditional-token-examples-py | Python     | Web3py条件代币示例      |
| amm-maths                     | TypeScript | AMM数学计算库           |
| withdrawal-checker            | TypeScript | 提款状态检查工具        |
| balance-checker               | TypeScript | 链上余额验证            |
| ctf-utils                     | TypeScript | CTF tokenId计算工具     |
| go-ctf-utils                  | Solidity   | Go版CTF工具             |

## 基础设施

| 项目                     | 语言       | 说明                          |
| ------------------------ | ---------- | ----------------------------- |
| erpc                     | Go         | 容错EVM RPC代理(fork)         |
| fx-portal                | Solidity   | Polygon跨链桥(fork)           |
| matic-withdrawal-batcher | Solidity   | Matic批量提款                 |
| relayer-deposits         | TypeScript | 自定义relayer充值             |
| polymarket-liq-mining    | TypeScript | 流动性挖矿奖励计算+Merkle分发 |
| status-page-front        | TypeScript | 基础设施状态页                |
| multi-endpoint-provider  | TypeScript | 多端点RPC provider            |
| matic-proofs             | TypeScript | Matic网络退出证明生成         |
| deploy-ctf               | TypeScript | CTF部署脚本                   |

## 审计/安全

| 项目              | 语言 | 说明                   |
| ----------------- | ---- | ---------------------- |
| contract-security | -    | 合约安全相关           |
| audit-checklist   | -    | Solidity审计清单       |
| solcurity         | -    | Solidity安全标准(fork) |

## Fork/依赖项目

| 项目                   | 语言       | 说明               |
| ---------------------- | ---------- | ------------------ |
| web3-react-multichain  | TypeScript | 多链web3-react框架 |
| s3x                    | Go         | IPFS S3 API(fork)  |
| forge-template         | Solidity   | Foundry模板        |
| go-ethereum-hdwallet   | Go         | HD钱包派生         |
| routing-api            | TypeScript | 路由API            |
| pyexchange             | Python     | 交易所Python API   |
| livepeerjs             | JavaScript | Livepeer JS库      |
| eztz                   | JavaScript | Tezos JS库         |
| ledger-cosmos-js       | JavaScript | Ledger Cosmos      |
| irisnet-crypto         | JavaScript | IRIS Hub加密库     |
| poly-py-eip712-structs | Python     | EIP712数据结构     |
| rmqprom                | Go         | RMQ Prometheus指标 |

## 其他

| 项目                               | 语言       | 说明                       |
| ---------------------------------- | ---------- | -------------------------- |
| polymarket-bounties                | Vue        | 赏金计划                   |
| polymarket-mev-bundle-poc          | TypeScript | MEV bundle PoC             |
| go-market-events                   | Go         | Go市场事件                 |
| go-redeemtions                     | Go         | Go赎回工具                 |
| redis-leaderboard                  | TypeScript | Redis排行榜                |
| leaderboard-username               | TypeScript | 排行榜用户名               |
| polymarket-liquidity-requests      | TypeScript | 流动性请求                 |
| polymarket-status-tool             | TypeScript | 状态工具                   |
| uma-ctf-adapter-sdk                | TypeScript | UMA适配器SDK               |
| uma-binary-adapter-sdk             | TypeScript | (deprecated) 二元适配器SDK |
| insta-exit-sdk                     | TypeScript | Biconomy快速退出SDK        |
| py-clob-client-l2-auth             | Python     | L2认证版CLOB客户端         |
| clob-client-l2-auth                | TypeScript | L2认证版CLOB客户端         |
| vue-components                     | JavaScript | Vue组件                    |
| cosmos-delegation-js               | JavaScript | Cosmos委托                 |
| tezbridge-crypto                   | JavaScript | Tezos加密工具              |
| infra-challenge-sre                | HCL        | SRE面试题                  |
| infra-challenge-devops             | Go         | DevOps面试题               |
| matic-withdrawal-batching-subgraph | TypeScript | Matic批量提款索引          |
| cachethq-docker                    | Shell      | Cachet Docker(fork)        |

---
