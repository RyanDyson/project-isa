"use client";

/**
 * Throwaway test page: submit orders / cancel / modify / feed prices
 * against the FastAPI backend and watch the book + trade tape respond,
 * per ticker. Not production UI — everything lives here on purpose.
 */

import { useCallback, useEffect, useState } from "react";

import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";

const BACKEND = "http://localhost:8000";

type BookData = {
  symbol: string;
  best_bid: number | null;
  best_ask: number | null;
  last_trade_price: number | null;
  bids: { price: number; orders: number; volume: number }[];
  asks: { price: number; orders: number; volume: number }[];
};

type TradeRow = {
  sequence: number;
  buy_order_id: number;
  sell_order_id: number;
  buy_agent: string;
  sell_agent: string;
  price: number;
  quantity: number;
  aggressor_side: string;
};

const ORDER_TYPES = ["market", "limit", "market-limit", "stop", "stop-limit"] as const;

export default function TestPage() {
  const [symbol, setSymbol] = useState("AAPL");
  const [knownSymbols, setKnownSymbols] = useState<string[]>(["AAPL"]);
  const [agentId, setAgentId] = useState("ui-agent");
  const [orderType, setOrderType] = useState<string>("limit");
  const [side, setSide] = useState<"buy" | "sell">("buy");
  const [quantity, setQuantity] = useState("10");
  const [limitPrice, setLimitPrice] = useState("10.5");
  const [stopPrice, setStopPrice] = useState("10.6");

  const [targetId, setTargetId] = useState("");
  const [feedPrice, setFeedPrice] = useState("10.55");

  const [book, setBook] = useState<BookData | null>(null);
  const [tape, setTape] = useState<TradeRow[]>([]);
  const [logLines, setLogLines] = useState<string[]>([]);

  const refreshSymbols = useCallback(async () => {
    const res = await fetch(`${BACKEND}/api/market/symbols`, { cache: "no-store" }).then(
      (r) => r.json(),
    );
    setKnownSymbols(res.symbols ?? []);
  }, []);

  const refresh = useCallback(async () => {
    const [bookRes, tapeRes, symbolsRes] = await Promise.all([
      fetch(`${BACKEND}/api/market/${symbol}/book?depth=10`, { cache: "no-store" }).then((r) =>
        r.json(),
      ),
      fetch(`${BACKEND}/api/market/${symbol}/trades?limit=50`, { cache: "no-store" }).then(
        (r) => r.json(),
      ),
      fetch(`${BACKEND}/api/market/symbols`, { cache: "no-store" }).then((r) => r.json()),
    ]);
    setBook(bookRes);
    setTape(tapeRes);
    setKnownSymbols(symbolsRes.symbols ?? []);
  }, [symbol]);

  useEffect(() => {
    void refresh();
    const interval = setInterval(() => void refresh(), 1500);
    return () => clearInterval(interval);
  }, [refresh]);

  const pushLog = (label: string, payload: unknown) => {
    const line = `[$${symbol}] ${label}: ${JSON.stringify(payload)}`;
    setLogLines((prev) => [line, ...prev].slice(0, 80));
  };

  const submitOrder = async () => {
    const body: Record<string, unknown> = {
      agent_id: agentId || "ui-agent",
      type: orderType,
      side,
      quantity: Number(quantity),
    };
    if (orderType !== "market") body.limit_price = Number(limitPrice);
    if (orderType === "stop" || orderType === "stop-limit") body.stop_price = Number(stopPrice);

    const res = await fetch(`${BACKEND}/api/market/${symbol}/orders`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify(body),
    }).then((r) => r.json());
    pushLog(`submit ${side} ${orderType}`, res);
    await refresh();
  };

  const cancelOrder = async () => {
    const id = Number(targetId);
    if (!id) return;
    const res = await fetch(`${BACKEND}/api/market/${symbol}/orders/${id}/cancel`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ agent_id: agentId || "ui-agent" }),
    }).then((r) => r.json());
    pushLog("cancel", res);
    await refresh();
  };

  const modifyOrder = async () => {
    const id = Number(targetId);
    if (!id) return;
    const body: Record<string, unknown> = { agent_id: agentId || "ui-agent" };
    if (quantity) body.quantity = Number(quantity);
    if (limitPrice && (orderType === "limit" || orderType === "stop-limit")) {
      body.limit_price = Number(limitPrice);
    }
    if (stopPrice && (orderType === "stop" || orderType === "stop-limit")) {
      body.stop_price = Number(stopPrice);
    }
    const res = await fetch(`${BACKEND}/api/market/${symbol}/orders/${id}`, {
      method: "PATCH",
      headers: { "content-type": "application/json" },
      body: JSON.stringify(body),
    }).then((r) => r.json());
    pushLog("modify", res);
    await refresh();
  };

  const pushPrice = async () => {
    const res = await fetch(`${BACKEND}/api/market/${symbol}/price`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ price: Number(feedPrice) }),
    }).then((r) => r.json());
    pushLog("price feed", res);
    await refresh();
  };

  return (
    <main className="bg-background min-h-screen p-6">
      <h1 className="mb-4 text-lg font-semibold">Order API Test Bench</h1>
      <p className="text-muted-foreground mb-4 text-xs">
        Direct tester against FastAPI on localhost:8000, per ticker. Throwaway
        page: books reset when the backend restarts.
      </p>

      <div className="mb-4 flex items-center gap-2">
        <span className="text-muted-foreground text-xs">ticker:</span>
        <select
          className="border-border bg-background rounded-md border p-1 text-xs"
          value={knownSymbols.includes(symbol) ? symbol : "__custom"}
          onChange={(e) => {
            if (e.target.value !== "__custom") setSymbol(e.target.value);
          }}
        >
          {knownSymbols.map((s) => (
            <option key={s} value={s}>
              ${s}
            </option>
          ))}
          {!knownSymbols.includes(symbol) && <option value="__custom">{symbol}</option>}
        </select>
        <Input
          className="w-28"
          value={symbol}
          onChange={(e) => setSymbol(e.target.value.toUpperCase())}
          placeholder="AAPL"
        />
        <Button size="sm" variant="outline" onClick={() => void refresh()}>
          Load
        </Button>
      </div>

      <div className="grid grid-cols-1 gap-4 lg:grid-cols-3">
        {/* order ticket */}
        <Card>
          <CardHeader>
            <CardTitle className="text-sm">Submit order</CardTitle>
          </CardHeader>
          <CardContent className="flex flex-col gap-2">
            <label className="text-muted-foreground text-xs">agent_id</label>
            <Input value={agentId} onChange={(e) => setAgentId(e.target.value)} />
            <label className="text-muted-foreground text-xs">type</label>
            <select
              className="border-border bg-background rounded-md border p-1 text-xs"
              value={orderType}
              onChange={(e) => setOrderType(e.target.value)}
            >
              {ORDER_TYPES.map((t) => (
                <option key={t} value={t}>
                  {t}
                </option>
              ))}
            </select>
            <div className="flex gap-2">
              <Button
                variant={side === "buy" ? "default" : "outline"}
                size="sm"
                className="flex-1"
                onClick={() => setSide("buy")}
              >
                Buy
              </Button>
              <Button
                variant={side === "sell" ? "secondary" : "outline"}
                size="sm"
                className="flex-1"
                onClick={() => setSide("sell")}
              >
                Sell
              </Button>
            </div>
            <label className="text-muted-foreground text-xs">quantity</label>
            <Input value={quantity} onChange={(e) => setQuantity(e.target.value)} />
            {orderType !== "market" && (
              <>
                <label className="text-muted-foreground text-xs">limit_price</label>
                <Input value={limitPrice} onChange={(e) => setLimitPrice(e.target.value)} />
              </>
            )}
            {(orderType === "stop" || orderType === "stop-limit") && (
              <>
                <label className="text-muted-foreground text-xs">stop_price</label>
                <Input value={stopPrice} onChange={(e) => setStopPrice(e.target.value)} />
              </>
            )}
            <Button onClick={() => void submitOrder()}>Submit</Button>
          </CardContent>
        </Card>

        {/* book */}
        <Card>
          <CardHeader>
            <CardTitle className="text-sm">
              ${book?.symbol ?? symbol} — last: {book?.last_trade_price ?? "—"} · bid{" "}
              {book?.best_bid ?? "—"} / ask {book?.best_ask ?? "—"}
            </CardTitle>
          </CardHeader>
          <CardContent>
            <div className="text-muted-foreground mb-1 text-xs uppercase">asks</div>
            <table className="w-full text-xs">
              <tbody>
                {[...(book?.asks ?? [])].reverse().map((row) => (
                  <tr key={`ask-${row.price}`} className="text-rose-400">
                    <td className="px-2 py-0.5">{row.price.toFixed(2)}</td>
                    <td className="px-2 py-0.5">{row.volume}</td>
                    <td className="text-muted-foreground px-2 py-0.5 text-right">×{row.orders}</td>
                  </tr>
                ))}
              </tbody>
            </table>
            <div className="bg-background/60 my-2 rounded px-2 py-1 text-center text-xs">
              spread{" "}
              {book && book.best_bid != null && book.best_ask != null
                ? (book.best_ask - book.best_bid).toFixed(2)
                : "—"}
            </div>
            <table className="w-full text-xs">
              <tbody>
                {(book?.bids ?? []).map((row) => (
                  <tr key={`bid-${row.price}`} className="text-emerald-400">
                    <td className="px-2 py-0.5">{row.price.toFixed(2)}</td>
                    <td className="px-2 py-0.5">{row.volume}</td>
                    <td className="text-muted-foreground px-2 py-0.5 text-right">×{row.orders}</td>
                  </tr>
                ))}
              </tbody>
            </table>
            <div className="mt-3">
              <Button size="xs" variant="outline" onClick={() => void refresh()}>
                Refresh now
              </Button>
            </div>
          </CardContent>
        </Card>

        {/* trade tape */}
        <Card>
          <CardHeader>
            <CardTitle className="text-sm">Trade tape</CardTitle>
          </CardHeader>
          <CardContent>
            <div className="max-h-48 overflow-y-auto">
              <table className="w-full text-xs">
                <tbody>
                  {tape.map((t) => (
                    <tr key={t.sequence}>
                      <td className="px-1 py-0.5">{t.sequence}</td>
                      <td className="px-1 py-0.5">{t.price.toFixed(2)}</td>
                      <td className="px-1 py-0.5">×{t.quantity}</td>
                      <td className="px-1 py-0.5">
                        {t.buy_agent} → {t.sell_agent}
                      </td>
                      <td className="px-1 py-0.5 text-right">{t.aggressor_side}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          </CardContent>
        </Card>

        {/* cancel / modify */}
        <Card>
          <CardHeader>
            <CardTitle className="text-sm">Order upkeep</CardTitle>
          </CardHeader>
          <CardContent className="flex flex-col gap-2">
            <label className="text-muted-foreground text-xs">target order_id</label>
            <Input value={targetId} onChange={(e) => setTargetId(e.target.value)} />
            <div className="flex gap-2">
              <Button size="sm" variant="outline" onClick={() => void cancelOrder()}>
                Cancel
              </Button>
              <Button size="sm" variant="outline" onClick={() => void modifyOrder()}>
                Modify (uses ticket fields)
              </Button>
            </div>
            <div className="border-border mt-2 rounded border p-2">
              <CardTitle className="text-xs">Price feed (stop tester)</CardTitle>
              <div className="mt-2 flex gap-2">
                <Input value={feedPrice} onChange={(e) => setFeedPrice(e.target.value)} />
                <Button size="sm" onClick={() => void pushPrice()}>
                  Feed
                </Button>
              </div>
            </div>
          </CardContent>
        </Card>

        {/* event log */}
        <Card>
          <CardHeader>
            <CardTitle className="text-sm">Engine events (latest first)</CardTitle>
          </CardHeader>
          <CardContent>
            <div className="max-h-72 overflow-y-auto font-mono text-[11px] leading-4">
              {logLines.map((line, index) => (
                <div key={`${index}-${line.slice(0, 12)}`} className="py-0.5">
                  {line}
                </div>
              ))}
            </div>
          </CardContent>
        </Card>
      </div>
    </main>
  );
}
