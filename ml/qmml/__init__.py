"""Training side of the machine-learning chantier (blueprint/wp/25-ml.md).

PyTorch trains; the C++ core evaluates (`quantModeling::ml::Mlp`). This
package is a development tool: nothing in the API or the production image
imports it.
"""
