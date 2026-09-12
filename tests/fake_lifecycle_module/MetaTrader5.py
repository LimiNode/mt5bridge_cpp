"""Dependency-free MetaTrader5 stub for owned interpreter lifecycle tests."""

def initialize():
    return True

def shutdown():
    return True

def last_error():
    return (1, "Success")

def terminal_info():
    return {"connected": True, "owned_interpreter": True}
