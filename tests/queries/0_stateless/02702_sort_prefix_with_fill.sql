-- { echoOn }
-- first row is less than WITH FILL FROM
-- select number from numbers(0, 1) order by number with fill from 1;

-- first row is bigger than WITH FILL FROM
-- select number from numbers(1, 1) order by number with fill from 0

-- sensor table
drop table if exists ts;
create table ts (sensor_id UInt64, timestamp UInt64, value Float64) ENGINE=MergeTree()  ORDER BY (sensor_id, timestamp);
insert into ts VALUES (1, 0, 1), (1, 2, 2), (3, 1, 1), (3, 3, 3); --, (4, '2021-12-01 00:00:01', 1), (4, '2021-12-01 00:00:05', 1);
select * from ts order by sensor_id, timestamp with fill step 1;
