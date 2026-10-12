import torch

# Networks of a few thousand weights: threads only add overhead, and one
# thread keeps a run reproducible.
torch.set_num_threads(1)
