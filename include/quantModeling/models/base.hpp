#ifndef MODELS_BASE_HPP
#define MODELS_BASE_HPP

#include <string>
namespace quantModeling
{

    struct IModel
    {
        virtual ~IModel() = default;
        virtual std::string model_name() const noexcept = 0;
    };
}

#endif