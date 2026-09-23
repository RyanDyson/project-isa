"""Agent pool foundations.

Agent types: market-making, momentum, spoofing-like, RL trading agent, and
the RL exchange planner. Stable-Baselines3 provides the RL algorithms;
Gymnasium defines how an agent observes, acts and receives reward. Agents
run concurrently via asyncio and submit orders through a Redis Streams
order queue to the single-writer matching engine.
"""

import gymnasium as gym


class MarketEnv(gym.Env):
    """Gymnasium environment wrapping the simulation core for RL agents."""

    # TODO: define observation/action spaces, reset(), step()
