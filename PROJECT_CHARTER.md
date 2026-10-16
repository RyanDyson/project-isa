# Project Charter: Agentic Trading Sandbox – Simulation Platform for Co-Adaptive Financial Markets

## General Project Information

| Field | Detail |
| --- | --- |
| Name | Project ISA – Agentic Trading Sandbox |
| Sponsor | Dr. Marcus Lam — Department of Computer Science, CityU HK |
| Start Date | [DD Month 2026] |
| Final Delivery | [DD Month 2027] |
| Project Manager | Daniel Wong |

## Project Overview

### Project Description

The project is a simulation platform for financial markets built as a two-level co-adaptive framework. The lower level is a population of trading agents (market-making, momentum, spoofing-like, and reinforcement-learning agents) that submit orders to a central limit order book (LOB) matching engine acting as a virtual exchange. The upper level is a reinforcement-learning exchange planner that observes the market as a whole and adjusts its policy in response to the population's evolving behaviour.

The platform mainly benefits [describe audience, e.g., researchers studying market microstructure, regulators evaluating market policy decisions] by providing a controlled, reproducible environment to simulate and evaluate market conditions and regulation strategies — without risking real market data, real capital, or real market participants.

### Business Needs

1. **Reproducible Market Experimentation:** Existing approaches to studying trading behaviour rely either on costly real-market observation or on simplistic, ad-hoc simulators. A purpose-built sandbox with persisted, replayable simulation logs (PostgreSQL) and a fast in-memory read layer (Redis) provides repeatability and analysis that ad-hoc simulations cannot.

2. **Correctness of Order Matching:** Most available tools and scripts mix market state management with agent logic, producing race conditions and untrustworthy results. This project enforces a strict separation between a single-writer matching engine and the many concurrent agents around it, so concurrency can never compromise the correctness of order matching (price-time priority).

3. **Evaluation of Adaptive Regulation:** Static frameworks fix the policy in advance; without a co-adaptive upper layer, one cannot empirically compare static and dynamic market-planning approaches. The RL planner closes this gap, reacting to population-level signals such as volatility, spread, liquidity depth, cancellation rate, and order-to-trade ratio.

## Scope

The project covers three layers of the system. At its centre is the **simulation core**, which comprises the limit order book (LOB) matching engine — implementing price-time priority matching as the single writer of the book — and the market metrics aggregator, which derives signals such as volatility, bid-ask spread, liquidity depth, trading volume, cancellation rate, and order-to-trade ratio. Feeding and recording this core is the **data and persistence layer**, which ingests market data through the Alpaca API (REST and WebSocket), generates historical replay and synthetic order flow as alternative data sources, and normalizes the resulting events into the engine's internal format; persistence is handled by a two-tier storage design, with Redis serving as the fast read layer and order queue, and PostgreSQL retaining long-term simulation logs for replay and analysis. Surrounding this is the **agent layer**, where market-making, momentum, spoofing-like, and reinforcement-learning agents run concurrently through asyncio and submit orders to the engine via the Redis Streams order queue, alongside the RL exchange planner that observes aggregate market conditions and adjusts its policy through its own queue.

The project will not cover live or algorithmic trading against real brokers or real money — Alpaca is used for market data only, with paper trading as a fallback — nor the development of a public-facing production brokerage or registration service. Specific boundaries of scope will be detailed through the Project Scope Statement, signed off one week after this charter.

## Deliverables

1. **POC Simulation Demo Release** — single-agent + basic LOB demo with dashboard visualization ([DD Month 2026])
2. **Full Platform Release** — full agent population, RL planner, metrics, dashboard ([DD Month 2027])

### Technology Stack

| Dashboard / Frontend | Version | Simulation Core / Backend | Version |
| --- | --- | --- | --- |
| Next.js + React | 15.2 / 19.0 | Python | 3.12 |
| TypeScript | 5.8 | NumPy, Pandas | 2.x / 2.2 |
| Better Auth React client | 1.3 | Stable-Baselines3, Gymnasium | 2.4 / 1.0 |
| TypeScript | 5.8 | FastAPI (API + auth), Uvicorn | 0.115 / 0.30 |
| — (all data via FastAPI backend) | — | SQLAlchemy ORM (auth/persistence) | 2.1 |
| Better Auth | 1.3 | asyncpg (PostgreSQL async driver) | 0.30 |
| Shadcn/ui + Tailwind CSS | 4.21 / 4.0 | asyncio (stdlib) + alpaca-py | 0.30+ |
| — (all data via FastAPI) | — | SQLAlchemy ORM (auth/persistence) | 2.1 |
| Bun (runtime / package manager) | 1.4.2 | uv (Python dependency management) | 0.12.23 |
| PostgreSQL (shared DB container) | 17.11 | Redis (fast read layer, order queue) | 7.4 |
| Docker Compose (local infra) | v5.5.1 | TensorBoard (experiment tracking) | — |

*Design constraint: all agents interact with the matching engine only through the order queue; no direct modification of the order book is allowed.*

## Project Resource Management

### Organization Assets

Project wiki / documentation (Confluence):

- Project Overview & Charter: `https://projectisa.atlassian.net/wiki/spaces/ISA/pages/140050231/Project+Overview`
- Simulation Core & LOB Engine Design: `https://projectisa.atlassian.net/wiki/spaces/ISA/pages/140050233/LOB+Engine+Design`
- Agent Pool & RL Planner Design: `https://projectisa.atlassian.net/wiki/spaces/ISA/pages/140050236/Agents+and+RL+Planner`
- API & Database Reference (Redis/PostgreSQL schemas): `https://projectisa.atlassian.net/wiki/spaces/ISA/pages/140050240/API+Database+Reference`
- Dashboard (Frontend) Guide: `https://projectisa.atlassian.net/wiki/spaces/ISA/pages/140050242/Dashboard+Guide`
- Development Setup & Onboarding: `https://projectisa.atlassian.net/wiki/spaces/ISA/pages/140050245/Development+Setup`

Project Repository: `github.com/projectisa/project-isa` (React/Next.js frontend + Python backend; `README.md` contains the architecture diagram and getting-started guide)

Development tooling:

- OpenCode — AI-assisted coding agent
- Docker Compose — one-command local infrastructure setup (`docker compose up -d`)
- TensorBoard — experiment tracking for RL training runs

### Budget Allocation

| Item | QTY | Total (HKD) |
| --- | --- | --- |
| Labour | 3 | HKD 180,000 |
| Cloud GPU training credits (RL agent/planner training) | 1 | HKD 2,000 |
| Cloud VM / hosting (long training and simulation experiment runs) | 1 | HKD 1,500 |
| Alpaca API (paper trading / basic data feed) | 1 | HKD 0 |
| TensorBoard / experiment tracking (free tier) | 1 | HKD 0 |
| **TOTAL** | | **HKD 183,500** (incl. labour) |

### Risk Management

| Risk | Mitigation |
| --- | --- |
| RL training instability / long convergence time | Cached/replayed datasets; TensorBoard early-experiment checks; budget allocation in the plan |
| Concurrency bugs compromise matching correctness | Single-writer LOB engine design; strict queue boundary; thorough test coverage of the engine |
| Simulator performance with many agents | Redis Streams queue + asyncio concurrency; benchmark early in scope |
| Alpaca API rate limits / missing historical data | REST fallback for historical recovery; synthetic data generator as fully offline fallback |
| Scope creep (dashboard features) | Deliverables fixed to the two releases; feature requests logged to backlog, not scope |

## Project Stakeholders

| Name | Role | Contact Email | Sign Off |
| --- | --- | --- | --- |
| Marcus Lam | Main Sponsor (Supervisor) | mlam@cityu.edu.hk | |
| Daniel Wong | Project Manager | dwong@cityu.edu.hk | |
| Ian Cheung | Programmer 1 | icheung@cityu.edu.hk | |
| Vivian Lee | Programmer 2 | vlee@cityu.edu.hk | |
| Oliver Hayes | Official Agent Representative | ohayes@gmail.com | |
