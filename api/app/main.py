from fastapi import FastAPI

from .pricing_service import price_asian, price_vanilla
from .schemas import AsianRequest, PricingResponse, VanillaRequest


app = FastAPI(title="quantModeling API", version="0.1.0")


@app.get("/health")
def health() -> dict:
    return {"status": "ok"}


@app.post("/price/vanilla", response_model=PricingResponse)
def price_vanilla_endpoint(req: VanillaRequest) -> PricingResponse:
    return price_vanilla(req)


@app.post("/price/asian", response_model=PricingResponse)
def price_asian_endpoint(req: AsianRequest) -> PricingResponse:
    return price_asian(req)
