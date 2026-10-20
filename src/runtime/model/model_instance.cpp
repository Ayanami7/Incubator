#include "runtime/model/model_instance.h"

#include <utility>

#include "runtime/core/error/error.h"

namespace Incubator::Model
{
    std::vector<Render::DrawItem> makeModelDraws(std::span<const ModelInstance> instances)
    {
        size_t count = 0;
        for (const auto& instance : instances)
        {
            if (!instance.model)
                throw Error::Exception(Error::Code::CONFIG, "ModelInstance requires a prepared model");
            count += instance.model->draws().size();
        }
        std::vector<Render::DrawItem> result;
        result.reserve(count);
        for (const auto& instance : instances)
        {
            for (auto draw : instance.model->draws())
            {
                draw.model = instance.transform * draw.model;
                result.push_back(std::move(draw));
            }
        }
        return result;
    }
}  // namespace Incubator::Model
