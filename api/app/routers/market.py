from fastapi import APIRouter

from .market_iv_surface import router as iv_surface_router
from .market_price_tape import router as price_tape_router
from .market_rates import router as rates_router

router = APIRouter()
router.include_router(price_tape_router)
router.include_router(iv_surface_router)
router.include_router(rates_router)
