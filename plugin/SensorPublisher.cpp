#include "SensorPublisher.h"

#include <geometry_msgs/PointStamped.h>
#include <geometry_msgs/QuaternionStamped.h>
#include <geometry_msgs/Vector3Stamped.h>
#include <std_msgs/Float64MultiArray.h>

#include <mujoco/mujoco.h>

#include <mujoco_ros_utils/ScalarStamped.h>
#include <mujoco_ros_utils/PointStampedMultiArray.h>
#include <mujoco_ros_utils/Vector3StampedMultiArray.h>

namespace MujocoRosUtils
{

void SensorPublisher::RegisterPlugin()
{
  mjpPlugin plugin;
  mjp_defaultPlugin(&plugin);

  plugin.name = "MujocoRosUtils::SensorPublisher";
  plugin.capabilityflags |= mjPLUGIN_SENSOR;

  const char * attributes[] = {"max_sensor_num", "sensor_name", "frame_id", "topic_name", "publish_rate"};

  plugin.nattribute = sizeof(attributes) / sizeof(attributes[0]);
  plugin.attributes = attributes;

  plugin.nstate = +[](const mjModel *, // m
                      int // plugin_id
                   ) { return 0; };

  plugin.nsensordata = +[](const mjModel *, // m
                           int, // plugin_id
                           int // sensor_id
                        ) { return 0; };

  // Can only run after forces have been computed
  plugin.needstage = mjSTAGE_ACC;

  plugin.init = +[](const mjModel * m, mjData * d, int plugin_id) {
    auto * plugin_instance = SensorPublisher::Create(m, d, plugin_id);
    if(!plugin_instance)
    {
      return -1;
    }
    d->plugin_data[plugin_id] = reinterpret_cast<uintptr_t>(plugin_instance);
    return 0;
  };

  plugin.destroy = +[](mjData * d, int plugin_id) {
    delete reinterpret_cast<SensorPublisher *>(d->plugin_data[plugin_id]);
    d->plugin_data[plugin_id] = 0;
  };

  plugin.reset = +[](const mjModel * m, double *, // plugin_state
                     void * plugin_data, int plugin_id) {
    auto * plugin_instance = reinterpret_cast<class SensorPublisher *>(plugin_data);
    plugin_instance->reset(m, plugin_id);
  };

  plugin.compute = +[](const mjModel * m, mjData * d, int plugin_id, int // capability_bit
                    ) {
    auto * plugin_instance = reinterpret_cast<class SensorPublisher *>(d->plugin_data[plugin_id]);
    plugin_instance->compute(m, d, plugin_id);
  };

  mjp_registerPlugin(&plugin);
}

SensorPublisher * SensorPublisher::Create(const mjModel * m, mjData * d, int plugin_id)
{
  // max_sensor_num
  const char * max_sensor_num_char = mj_getPluginConfig(m, plugin_id, "max_sensor_num");
  int max_sensor_num = strlen(max_sensor_num_char) == 0 ? 1 : atoi(max_sensor_num_char);
  if(max_sensor_num <= 0)
  {
    mju_error("[SensorPublisher] `max_sensor_num` must be positive.");
    return nullptr;
  }
  std::cout << "[SensorPublisher] max_sensor_num: " << max_sensor_num << std::endl;

  // sensor_name
  const char * sensor_name_char = mj_getPluginConfig(m, plugin_id, "sensor_name");
  if(strlen(sensor_name_char) == 0)
  {
    mju_error("[SensorPublisher] `sensor_name` is missing.");
    return nullptr;
  }

  std::cout << "[SensorPublisher] sensor_name: " << sensor_name_char << std::endl;
  int sensor_id = 0;
  std::optional<std::vector<int>> sensor_id_list;
  for(; sensor_id < m->nsensor; sensor_id++)
  {
    if(max_sensor_num > 1)
    {
      if(strstr(mj_id2name(m, mjOBJ_SENSOR, sensor_id), sensor_name_char))
      {
        if(!sensor_id_list.has_value())
        {
          sensor_id_list = std::vector<int>();
        }
        sensor_id_list.value().push_back(sensor_id);
      }
    }
    else
    {
      if(strcmp(sensor_name_char, mj_id2name(m, mjOBJ_SENSOR, sensor_id)) == 0)
      {
        break;
      }
    }
  }

  // check for sensor num
  if(max_sensor_num != sensor_id_list.value().size())
  {
    std::cerr << "[SensorPublisher] The defined sensor is " << max_sensor_num << std::endl;
    std::cerr << "[SensorPublisher] The number of finded sensor is " << sensor_id_list.value().size() << std::endl;
    mju_error("[SensorPublisher] The number of sensor is invalid.");
    return nullptr;
  }

  // msg_type
  MessageType msg_type;
  int sensor_dim;
  // input final sensor id when max_sensor_num > 1
  if(max_sensor_num > 1)
  {
    if(!(std::find(sensor_id_list.value().begin(), sensor_id_list.value().end(), sensor_id)
         == sensor_id_list.value().end()))
    {
      mju_error("[SensorPublisher] The sensors with the specified name not found.");
      return nullptr;
    }
    {
      //check sensor dim in list
      std::vector<int> sensor_dim_list;
      for(int i = 0; i < max_sensor_num; i++)
      {
        sensor_dim_list.push_back(m->sensor_dim[sensor_id_list.value()[i]]);
      }
      if(std::adjacent_find(sensor_dim_list.begin(), sensor_dim_list.end(), std::not_equal_to<int>())
         != sensor_dim_list.end())
      {
        mju_error("[SensorPublisher] The sensors with the specified name have different dimensions.");
        return nullptr;
      }
      sensor_dim = m->sensor_dim[sensor_id_list.value()[0]];

      // set msg_type
      if(sensor_dim == 1)
      {
        msg_type = MsgScalar_ARRAY;
      }
      else if(sensor_dim == 3)
      {
        if(m->sensor_type[sensor_id] == mjSENS_FRAMEPOS)
        {
          msg_type = MsgPoint_ARRAY;
        }
        else
        {
          msg_type = MsgVector3_ARRAY;
        }
      }
      else if(sensor_dim == 4)
      {
        msg_type = MsgQuaternion;
      }
      else
      {
        mju_error("[SensorPublisher] Unsupported sensor data dimensions: %d.", sensor_dim);
        return nullptr;
      }
    }
  }
  else
  {
    if(sensor_id == m->nsensor)
    {
      mju_error("[SensorCommand] The sensor with the specified name not found.");
      return nullptr;
    }
    sensor_dim = m->sensor_dim[sensor_id];
    if(sensor_dim == 1)
    {
      msg_type = MsgScalar;
    }
    else if(sensor_dim == 3)
    {
      if(m->sensor_type[sensor_id] == mjSENS_FRAMEPOS)
      {
        msg_type = MsgPoint;
      }
      else
      {
        msg_type = MsgVector3;
      }
    }
    else if(sensor_dim == 4)
    {
      msg_type = MsgQuaternion;
    }
    else
    {
      mju_error("[SensorPublisher] Unsupported sensor data dimensions: %d.", sensor_dim);
      return nullptr;
    }
  }

  // frame_id
  const char * frame_id_char = mj_getPluginConfig(m, plugin_id, "frame_id");
  std::string frame_id = "";
  if(strlen(frame_id_char) > 0)
  {
    frame_id = std::string(frame_id_char);
  }

  // topic_name
  const char * topic_name_char = mj_getPluginConfig(m, plugin_id, "topic_name");
  std::string topic_name = "";
  if(strlen(topic_name_char) > 0)
  {
    topic_name = std::string(topic_name_char);
  }

  // publish_rate
  const char * publish_rate_char = mj_getPluginConfig(m, plugin_id, "publish_rate");
  mjtNum publish_rate = 30.0;
  if(strlen(publish_rate_char) > 0)
  {
    publish_rate = strtod(publish_rate_char, nullptr);
  }
  if(publish_rate <= 0)
  {
    mju_error("[SensorPublisher] `publish_rate` must be positive.");
    return nullptr;
  }

  std::cout << "[SensorPublisher] Create." << std::endl;

  if(sensor_id_list.has_value())
  {
    return new SensorPublisher(m, d, sensor_id, msg_type, frame_id, topic_name, publish_rate, sensor_id_list.value());
  }

  return new SensorPublisher(m, d, sensor_id, msg_type, frame_id, topic_name, publish_rate);
}

SensorPublisher::SensorPublisher(const mjModel * m,
                                 mjData *, // d,
                                 int sensor_id,
                                 MessageType msg_type,
                                 const std::string & frame_id,
                                 const std::string & topic_name,
                                 mjtNum publish_rate,
                                 const std::vector<int> & sensor_id_list)
: sensor_id_(sensor_id), msg_type_(msg_type), frame_id_(frame_id), topic_name_(topic_name),
  publish_skip_(std::max(static_cast<int>(1.0 / (publish_rate * m->opt.timestep)), 1)), sensor_id_list_(sensor_id_list)
{
  if(frame_id_.empty())
  {
    frame_id_ = "map";
  }
  if(topic_name_.empty())
  {
    std::string sensor_name = std::string(mj_id2name(m, mjOBJ_SENSOR, sensor_id_));
    topic_name_ = "mujoco/" + sensor_name;
  }

  int argc = 0;
  char ** argv = nullptr;
  if(!ros::isInitialized())
  {
    ros::init(argc, argv, "mujoco_ros", ros::init_options::NoSigintHandler);
  }

  nh_ = std::make_shared<ros::NodeHandle>();
  if(msg_type_ == MsgScalar)
  {
    pub_ = nh_->advertise<mujoco_ros_utils::ScalarStamped>(topic_name_, 1);
  }
  else if(msg_type_ == MsgScalar_ARRAY)
  {
    pub_ = nh_->advertise<std_msgs::Float64MultiArray>(topic_name_, 1);
  }
  else if(msg_type_ == MsgPoint)
  {
    pub_ = nh_->advertise<geometry_msgs::PointStamped>(topic_name_, 1);
  }
  else if(msg_type_ == MsgPoint_ARRAY)
  {
    pub_ = nh_->advertise<mujoco_ros_utils::PointStampedMultiArray>(topic_name_, 1);
  }
  else if(msg_type_ == MsgVector3)
  {
    pub_ = nh_->advertise<geometry_msgs::Vector3Stamped>(topic_name_, 1);
  }
  else if(msg_type_ == MsgVector3_ARRAY)
  {
    pub_ = nh_->advertise<mujoco_ros_utils::Vector3StampedMultiArray>(topic_name_, 1);
  }
  else // if(msg_type_ == MsgQuaternion)
  {
    pub_ = nh_->advertise<geometry_msgs::QuaternionStamped>(topic_name_, 1);
  }
}

void SensorPublisher::reset(const mjModel *, // m
                            int // plugin_id
)
{
}

void SensorPublisher::compute(const mjModel * m, mjData * d, int // plugin_id
)
{
  sim_cnt_++;
  if(sim_cnt_ % publish_skip_ != 0)
  {
    return;
  }

  std_msgs::Header header;
  header.stamp = ros::Time::now();
  header.frame_id = frame_id_;

  int sensor_adr = m->sensor_adr[sensor_id_];
  if(msg_type_ == MsgScalar)
  {
    mujoco_ros_utils::ScalarStamped msg;
    msg.header = header;
    msg.value = d->sensordata[sensor_adr];
    pub_.publish(msg);
  }
  else if(msg_type_ == MsgScalar_ARRAY)
  {
    std_msgs::Float64MultiArray msg;
    msg.data.resize(sensor_id_list_.size());
    for(size_t i = 0; i < sensor_id_list_.size(); i++)
    {
      sensor_adr = m->sensor_adr[sensor_id_list_[i]];
      msg.data[i] = d->sensordata[sensor_adr];
    }
    pub_.publish(msg);
  }
  else if(msg_type_ == MsgPoint)
  {
    geometry_msgs::PointStamped msg;
    msg.header = header;
    msg.point.x = d->sensordata[sensor_adr + 0];
    msg.point.y = d->sensordata[sensor_adr + 1];
    msg.point.z = d->sensordata[sensor_adr + 2];
    pub_.publish(msg);
  }
  else if(msg_type_ == MsgPoint_ARRAY)
  {
    mujoco_ros_utils::PointStampedMultiArray msg;
    msg.points.resize(sensor_id_list_.size());
    msg.header = header;
    for(size_t i = 0; i < sensor_id_list_.size(); i++)
    {
      sensor_adr = m->sensor_adr[sensor_id_list_[i]];

      geometry_msgs::Point point;
      point.x = d->sensordata[sensor_adr + 0];
      point.y = d->sensordata[sensor_adr + 1];
      point.z = d->sensordata[sensor_adr + 2];
      msg.points[i] = point;
    }
    pub_.publish(msg);
  }
  else if(msg_type_ == MsgVector3)
  {
    geometry_msgs::Vector3Stamped msg;
    msg.header = header;
    msg.vector.x = d->sensordata[sensor_adr + 0];
    msg.vector.y = d->sensordata[sensor_adr + 1];
    msg.vector.z = d->sensordata[sensor_adr + 2];
    pub_.publish(msg);
  }
  else if(msg_type_ == MsgVector3_ARRAY)
  {
    mujoco_ros_utils::Vector3StampedMultiArray msg;
    msg.x.resize(sensor_id_list_.size());
    msg.y.resize(sensor_id_list_.size());
    msg.z.resize(sensor_id_list_.size());
    msg.header = header;
    for(size_t i = 0; i < sensor_id_list_.size(); i++)
    {
      sensor_adr = m->sensor_adr[sensor_id_list_[i]];

      msg.x[i] = d->sensordata[sensor_adr + 0];
      msg.y[i] = d->sensordata[sensor_adr + 1];
      msg.z[i] = d->sensordata[sensor_adr + 2];
    }
    pub_.publish(msg);
  }
  else // if(msg_type_ == MsgQuaternion)
  {
    geometry_msgs::QuaternionStamped msg;
    msg.header = header;
    msg.quaternion.w = d->sensordata[sensor_adr + 0];
    msg.quaternion.x = d->sensordata[sensor_adr + 1];
    msg.quaternion.y = d->sensordata[sensor_adr + 2];
    msg.quaternion.z = d->sensordata[sensor_adr + 3];
    pub_.publish(msg);
  }
}

} // namespace MujocoRosUtils
